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
                    SweepResult swept = sweep(replay.engine, replay.board, store,
                        TERMINAL_INV_TEMP);
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
                  << "Hand meta misses: " << metaMisses << std::endl;
        if (mismatches != 0 || legalMismatches != 0 || metaMisses != 0) {
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
