// Proves that replaying an action list reconstructs exactly the state key the
// solver would have produced while playing that hand.
//
//   build/keyEquivalence [hands] [sweepEvery]
//
// A real Engine self plays, sampling uniformly from the legal options. At every
// node it records the action list so far, the board for the street it is on, the
// acting seat's hole cards, and getState(). Each of those is then rebuilt through
// Replay + comboKey, which knows nothing about that hand beyond the actions and
// the board - villains get placeholder cards, and the whole future runout is
// withheld.
//
// This is what the abstraction skill prescribes: validate against the C++ rather
// than against reasoning. A wrong key does not error, it silently returns another
// state's weights.

#include "solver/config.h"
#include "server/replay.h"
#include "server/sweep.h"

#include <cmath>
#include <cstdlib>

struct Node {
    std::vector<int> actions;
    std::vector<int> board;
    int seat;
    int first;
    int second;
    uint64_t key;
    std::array<bool, 5> legal;
};

// Every component keyComponents() reports must equal the bit field getState()
// actually wrote. It is the only code in server/ that reproduces the encoding
// rather than calling it, so without this a shifted field or a wrong bucket
// boundary would return a plausible number and nothing would complain.
//
// Returns the number of mismatches, naming the first few.
static int checkComponents(const Engine& engine, uint64_t key, uint64_t& reported) {
    auto field = [&](int shift, int width) {
        return int((key >> shift) & ((1ULL << width) - 1));
    };
    KeyComponents parts = keyComponents(engine);
    int bad = 0;
    auto expect = [&](const char* name, int got, int want) {
        if (got == want) {
            return;
        }
        bad++;
        if (reported < 10) {
            reported++;
            std::cerr << "component " << name << ": built " << got << ", key holds "
                      << want << " (round " << int(engine.round) << ")" << std::endl;
        }
    };

    expect("roundHeadsBasic", parts.roundHeadsBasic, field(0, 2));
    expect("headsBasic", parts.headsBasic, field(2, 2));
    expect("actionPositionBucket", parts.actionPositionBucket, field(4, 2));
    expect("sprCprBucket", parts.sprCprBucket, field(6, 4));
    // Preflop the board field carries the hand dependent 169 index, so there is
    // nothing shared to compare against.
    if (parts.hasBoardField) {
        expect("boardField", parts.boardField, field(10, 15));
    }
    for (int i = 0; i < 4; i++) {
        expect("playerLastActions", parts.playerLastActions[i], field(38 + i, 1));
        expect("villanLastActions", parts.villanLastActions[i], field(42 + i, 1));
    }
    expect("playerPositionBasic", parts.playerPositionBasic, field(46, 3));
    expect("villanPositionBasic", parts.villanPositionBasic, field(49, 3));
    expect("raiseSizeOverZero", parts.raiseSizeOverZero, field(52, 1));
    expect("raiseStrength", parts.raiseStrength, field(53, 2));
    expect("hiddenRaiseSizeOverZero", parts.hiddenRaiseSizeOverZero, field(55, 1));
    expect("hiddenRaiseStrength", parts.hiddenRaiseStrength, field(56, 2));
    expect("raisesFaced", parts.raisesFaced, field(58, 2));
    expect("relativePosition", parts.relativePosition, field(60, 1));
    expect("playerCallsOverOne", parts.playerCallsOverOne ? 1 : 0, field(61, 1));
    expect("calls", parts.calls, field(62, 2));

    // raiseHistory replaces raiseHistoryCompressed in the response, so prove the
    // four counts are the real ones by rebuilding the colex rank from them.
    std::array<std::array<uint32_t, 8>, 53> binomials = engine.binomials;
    const std::array<uint8_t, 4>& r = engine.raiseHistory;
    int compressed = 0;
    for (int street = 0; street < 4; street++) {
        for (int n = 0; n < parts.raiseHistory[street]; n++) {
            // The increment uses the counts as they stood at that raise.
            int r0 = street == 0 ? n : r[0];
            int r1 = street == 1 ? n : (street > 1 ? r[1] : 0);
            int r2 = street == 2 ? n : (street > 2 ? r[2] : 0);
            compressed += street == 0 ? int(binomials[9 - r0][3])
                        : street == 1 ? int(binomials[8 - r0 - r1][2])
                        : street == 2 ? (7 - r0 - r1 - r2)
                                      : 1;
        }
    }
    expect("raiseHistory -> compressed", compressed, field(30, 8));
    return bad;
}

// Every option getOptions() marks illegal must have an explanation. An option that
// falls through every clause in illegalReason() would otherwise be rejected by the
// API with no reason at all.
static int checkReasons(Engine& engine, const std::array<bool, 5>& legal) {
    int bad = 0;
    for (int j = 0; j < 5; j++) {
        if (!legal[j] && illegalReason(engine, j).empty()) {
            bad++;
        }
    }
    return bad;
}

int main(int argc, char** argv) {
    try {
        uint64_t hands = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
        uint64_t sweepEvery = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 500;
        // The walk is shaped to reach later streets, not to be realistic.
        // Sampling uniformly folds nearly every hand out preflop; simply
        // discounting fold is worse, because the tree then raises to the six raise
        // cap and the all in short circuit in newRound() ends the hand before the
        // flop. Favouring calls and the small raise is what actually buys turn and
        // river nodes, where the board class, hand meta and cross street raise
        // descriptors live.
        std::array<double, 5> actionWeights = {0.6, 3.0, 1.0, 0.25, 0.05};

        Engine::loadTables(false);
        StrategyStore store;
        store.open(defaultStrategyPath(), defaultIndexPath());
        std::cout << "Strategy: " << store.records << " records" << std::endl;

        Engine engine;
        std::mt19937 gen(20260901);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        Replay replay;
        std::array<uint64_t, 4> nodesByRound = {};
        uint64_t checked = 0;
        uint64_t sweeps = 0;
        uint64_t mismatches = 0;
        uint64_t legalMismatches = 0;
        uint64_t metaMisses = 0;
        uint64_t componentMismatches = 0;
        uint64_t reasonGaps = 0;
        uint64_t componentReports = 0;

        for (uint64_t hand = 0; hand < hands; hand++) {
            engine.shuffle(gen);
            engine.deal();
            std::vector<int> actions;
            std::vector<Node> nodes;
            int option;
            do {
                uint64_t key = engine.getState(METAS_BASIC, MEMORY_BASIC);
                std::array<bool, 5> legal = engine.getOptions();
                int seat = engine.getAction();
                Node node;
                node.actions = actions;
                node.board.assign(engine.boardCards.begin(),
                    engine.boardCards.begin() + roundBoardCards(engine.round));
                node.seat = seat;
                node.first = engine.players[seat].holeCards[0];
                node.second = engine.players[seat].holeCards[1];
                node.key = key;
                node.legal = legal;
                nodes.push_back(std::move(node));
                nodesByRound[engine.round]++;

                // Flat over the non fold options, so the walk covers lines the
                // trained policy would almost never take.
                std::array<double, 5> draws = {};
                double total = 0;
                for (int j = 0; j < 5; j++) {
                    if (legal[j]) {
                        draws[j] = actionWeights[j];
                    }
                    total += draws[j];
                }
                double sample = unit(gen) * total;
                option = 4;
                double running = 0;
                for (int j = 0; j < 5; j++) {
                    running += draws[j];
                    if (sample < running) {
                        option = j;
                        break;
                    }
                }
                actions.push_back(option);
            } while (engine.play(option));

            // The board the query carries is the street's board, so a flop node
            // sorts its three cards the same way deal() does. Comparing against
            // the engine's own board slice keeps that honest.
            for (const Node& node : nodes) {
                ReplayResult result = replay.run(node.actions, node.board);
                if (!result.ok) {
                    std::cerr << "replay failed at hand " << hand << ": " << result.error
                              << " (action " << result.actionIndex << ")" << std::endl;
                    return 1;
                }
                if (replay.engine.getAction() != node.seat) {
                    std::cerr << "replay reached seat " << int(replay.engine.getAction())
                              << ", expected " << node.seat << std::endl;
                    return 1;
                }
                if (result.legal != node.legal) {
                    legalMismatches++;
                }
                uint64_t key = 0;
                if (!comboKey(replay.engine, node.first, node.second, key)) {
                    metaMisses++;
                    continue;
                }
                componentMismatches += checkComponents(replay.engine, key,
                    componentReports);
                reasonGaps += checkReasons(replay.engine, result.legal);
                if (key != node.key) {
                    if (mismatches < 5) {
                        std::cerr << "key mismatch at hand " << hand << " round "
                                  << int(replay.engine.round) << " seat " << node.seat
                                  << ": replay " << std::hex << key << " engine "
                                  << node.key << std::dec << " (differing bits "
                                  << std::hex << (key ^ node.key) << std::dec << ")"
                                  << std::endl;
                    }
                    mismatches++;
                }
                checked++;

                // The full sweep is 1326 classifications, so it runs on a sample.
                // It has to agree with the targeted check on the hero's own row,
                // and its combo accounting has to add up.
                if (checked % sweepEvery == 0) {
                    // Every field on, so the sweep does all the work it can.
                    SweepFields allFields{true, true, true, true, true, true};
                    SweepResult swept = sweep(replay.engine, replay.board, store,
                        TERMINAL_INV_TEMP, allFields);
                    sweeps++;
                    const ComboRow& row = swept.rows[holePairIndex(node.first, node.second)];
                    if (!row.valid || row.key != node.key) {
                        std::cerr << "sweep row disagrees with the engine key" << std::endl;
                        return 1;
                    }
                    int available = 52 - int(node.board.size());
                    int expectedValid = available * (available - 1) / 2;
                    if (swept.invalid != COMBO_COUNT - expectedValid) {
                        std::cerr << "sweep counted " << swept.invalid
                                  << " invalid combos, expected "
                                  << (COMBO_COUNT - expectedValid) << std::endl;
                        return 1;
                    }
                    if (swept.found + swept.missing + swept.invalid + swept.metaMissing
                            != COMBO_COUNT) {
                        std::cerr << "sweep counts do not sum to 1326" << std::endl;
                        return 1;
                    }
                    // Frequencies are a distribution over the legal options only.
                    for (const ComboRow& each : swept.rows) {
                        if (!each.found) {
                            continue;
                        }
                        double sum = 0;
                        for (int j = 0; j < 5; j++) {
                            if (!swept.legal[j] && each.frequencies[j] != 0.0) {
                                std::cerr << "illegal option carries frequency "
                                          << each.frequencies[j] << std::endl;
                                return 1;
                            }
                            sum += each.frequencies[j];
                        }
                        if (std::abs(sum - 1.0) > 1e-9) {
                            std::cerr << "frequencies sum to " << sum << std::endl;
                            return 1;
                        }
                    }
                }
            }
        }

        std::cout << "Hands: " << hands << "\n"
                  << "Nodes checked: " << checked << "\n"
                  << "  preflop " << nodesByRound[0] << ", flop " << nodesByRound[1]
                  << ", turn " << nodesByRound[2] << ", river " << nodesByRound[3] << "\n"
                  << "Full sweeps: " << sweeps << "\n"
                  << "Key mismatches: " << mismatches << "\n"
                  << "Legal set mismatches: " << legalMismatches << "\n"
                  << "Hand meta misses: " << metaMisses << "\n"
                  << "Key component mismatches: " << componentMismatches << "\n"
                  << "Unexplained illegal options: " << reasonGaps << std::endl;
        if (mismatches != 0 || legalMismatches != 0 || metaMisses != 0
                || componentMismatches != 0 || reasonGaps != 0) {
            std::cerr << "FAILED" << std::endl;
            return 1;
        }
        std::cout << "PASSED" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
