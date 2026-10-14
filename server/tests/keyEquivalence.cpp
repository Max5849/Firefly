// Proves the server reconstructs exactly what the trainer's engine holds.
//
//   build/keyEquivalence [hands]
//
// Part 1, lockstep. The trainer's own Engine (solver/multiHand, PLAYER_COUNT seats,
// CHIP_COUNT chips) and the server's TableEngine<PLAYER_COUNT> play the same hands side
// by side. At every decision both acting ranges are narrowed by the same random
// per-group policy - the trainer's through a copy of Explorer::chooseRangeOption's
// range pass, the server's through TableEngine::applyPolicy - and the two engines must
// agree on the action state, the options, every seat's chips, bets and range, what
// play() and beginNewRound() return, and the meta state of every combo the trainer
// classifies.
//
// Part 2, every table size the server dispatches on (3..7). TableEngine self plays
// under random metasBasic / memoryBasic / stacks, calls weighted up so the walk
// reaches the river, and at every node:
//   - Replay, given nothing but the action list and the revealed board, reaches the
//     same action state, options and meta state;
//   - every field decodeKey reports equals the engine quantity it encodes, and the
//     fields repack into the shared key;
//   - every option getOptions() turns off has a reason, and no legal one does;
//   - a raise lands on the bet TableEngine::betTo promised.
//
// A wrong key does not error, it silently returns another state's weights, so this is
// validated against the engines rather than against reasoning.

#include "server/replay.h"
#include "server/keyLayout.h"

#include <cmath>
#include <cstdlib>

struct Tally {
    uint64_t nodes = 0;
    std::array<uint64_t, 4> perRound = {};
    uint64_t failures = 0;
};

static void report(Tally& tally, const std::string& what) {
    tally.failures++;
    if (tally.failures <= 20) {
        std::cerr << what << std::endl;
    }
}

// The SPR / CPR bucket setActionState() writes, worked out independently.
static int expectedSprCpr(float spr, float cpr, bool memoryBasic) {
    if (spr < 0.0005f) {
        const int cuts = memoryBasic ? 3 : 6;
        for (int i = 0; i < cuts; i++) {
            const int t = memoryBasic ? 2 * i : i;
            if (cpr < SPR_THRESHOLDS[t]) {
                return 9 + (memoryBasic ? 2 * i : i);
            }
        }
        return 15;
    }
    const int cuts = memoryBasic ? 4 : 8;
    for (int i = 0; i < cuts; i++) {
        const int t = memoryBasic ? 2 * i : i;
        if (spr < SPR_THRESHOLDS[t]) {
            return memoryBasic ? 2 * i : i;
        }
    }
    return 8;
}

static int bucket(int x) {
    return (x > 0) + (x > 2);
}

template <int P>
static void checkDecode(const Replay<P>& replay, Tally& tally, const std::string& where) {
    const KeyLayout layout = decodeKey(replay);
    const auto& engine = replay.engine;
    const bool mb = replay.config.memoryBasic;
    const auto& hero = engine.players[engine.action];
    const auto& villan = engine.players[hero.villanPosition];
    std::map<std::string, const KeyField*> byName;
    uint64_t repacked = 0;
    for (const KeyField& field : layout.fields) {
        byName[field.name] = &field;
        repacked |= field.value << field.shift;
    }
    if (repacked != layout.sharedKey) {
        report(tally, where + ": fields do not repack into the shared key");
    }
    auto expect = [&](const std::string& name, int got, int want) {
        if (got != want) {
            report(tally, where + ": " + name + " decoded " + std::to_string(got)
                + ", engine holds " + std::to_string(want));
        }
    };
    expect("headsBasic", int(byName["headsBasic"]->value), engine.headsBasic);
    expect("sprCpr", int(byName["sprCpr"]->value), expectedSprCpr(hero.spr, hero.cpr, mb));
    for (int street = 0; street < 4; street++) {
        expect("raiseHistory", byName["raiseHistory"]->parts[street].value, engine.raiseHistory[street]);
        const int d = street != engine.round
            ? 2 * hero.lastActions[street] + !mb * villan.lastActions[street]
            : hero.lastActions[street] + !mb * villan.lastActions[street];
        expect("story digit", byName["story"]->parts[street].value, d);
    }
    const bool keepPositions = !mb || engine.round < 1;
    expect("hero position", byName["story"]->parts[4].value, keepPositions * hero.positionBasic);
    expect("villain position", byName["story"]->parts[5].value, keepPositions * villan.positionBasic);
    expect("to act", byName["actionPositions"]->parts[0].value, bucket(engine.actionPosition));
    expect("raiser position", byName["actionPositions"]->parts[1].value, bucket(engine.raiseActionPosition));
    expect("hidden position", byName["actionPositions"]->parts[2].value,
        !mb * bucket(engine.hiddenRaiseActionPosition));
    expect("raiseSize", int(byName["raiseSize"]->value), engine.raiseSize > 0);
    expect("raiseStrength", int(byName["raiseStrength"]->value), std::min<int>(engine.raiseStrength, 3));
    expect("hiddenRaiseSize", int(byName["hiddenRaiseSize"]->value), !mb * (engine.hiddenRaiseSize > 0));
    expect("hiddenRaiseStrength", int(byName["hiddenRaiseStrength"]->value),
        !mb * std::min<int>(engine.hiddenRaiseStrength, 3));
    expect("raisesFaced", int(byName["raisesFaced"]->value), std::min<int>(hero.raisesFaced, 3));
    expect("relativePosition", int(byName["relativePosition"]->value), hero.relativePosition);
    expect("heroCalls", int(byName["heroCalls"]->value), hero.calls > 1);
    const int tableCalls = engine.calls < 2 ? engine.calls : engine.calls < 4 ? 2 : 3;
    expect("tableCalls", int(byName["tableCalls"]->value), !mb * tableCalls);
    if (engine.round > 0) {
        const int boardClass = replay.config.metasBasic ? engine.boardClassBasic : engine.boardClass;
        expect("boardField", int(byName["boardField"]->value), boardClass + 169);
    }
}

// The range pass of Explorer::chooseRangeOption (explorer.cpp), less the card removal
// sums, which no key reads: multiply by the group's probability, then floor.
static void trainerNarrow(Engine& engine, int seat, const std::array<uint8_t, COMBO_COUNT>& groups,
        const std::array<std::array<double, 5>, GROUP_COUNT>& policy, int option) {
    std::array<float, COMBO_COUNT>& range = engine.getRange(seat);
    double rangeSum = 0;
    for (int combo = 0; combo < COMBO_COUNT; combo++) {
        const uint8_t group = groups[combo];
        if (group == NO_GROUP) {
            continue;
        }
        range[combo] *= float(policy[group][option]);
        rangeSum += range[combo];
    }
    const double floor = RANGE_FLOOR * rangeSum;
    for (int combo = 0; combo < COMBO_COUNT; combo++) {
        if (groups[combo] != NO_GROUP && range[combo] < floor) {
            range[combo] = 0.0f;
        }
    }
}

// The acting seat's groups, as chooseRangeOption forms them: combos off the revealed
// board with reach left.
template <typename E>
static std::array<uint8_t, COMBO_COUNT> actingGroups(E& engine, bool metasBasic) {
    std::array<uint8_t, COMBO_COUNT> groups;
    groups.fill(NO_GROUP);
    const auto& range = engine.players[engine.action].range;
    for (int combo = 0; combo < COMBO_COUNT; combo++) {
        if (engine.commonRange[combo] && range[combo] > 0.0f) {
            groups[combo] = engine.getGroup(metasBasic, combo);
        }
    }
    return groups;
}

static void lockstep(uint64_t hands, Tally& tally) {
    std::mt19937 gen(7);
    auto trainer = std::make_unique<Engine>();
    auto table = std::make_unique<TableEngine<PLAYER_COUNT>>();
    table->startingChips = CHIP_COUNT;
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<int> coin(0, 1);

    auto compareSeats = [&](const std::string& where) {
        for (int seat = 0; seat < PLAYER_COUNT; seat++) {
            const auto& a = trainer->players[seat];
            const auto& b = table->players[seat];
            if (a.chips != b.chips || a.bet != b.bet || a.inHand != b.inHand) {
                report(tally, where + ": seat " + std::to_string(seat) + " chips, bet or inHand differ");
            }
            if (std::memcmp(a.range.data(), b.range.data(), sizeof(float) * COMBO_COUNT) != 0) {
                report(tally, where + ": seat " + std::to_string(seat) + " range differs");
            }
        }
        if (trainer->pot != table->pot || trainer->bet != table->bet || trainer->round != table->round
                || trainer->action != table->action || trainer->heads != table->heads) {
            report(tally, where + ": pot, bet, round, action or heads differ");
        }
    };

    for (uint64_t hand = 0; hand < hands; hand++) {
        const bool metasBasic = coin(gen);
        trainer->shuffle(gen);
        trainer->deal();
        table->deal({trainer->deck[0], trainer->deck[1], trainer->deck[2],
            trainer->deck[3], trainer->deck[4]});
        int decisions = 0;
        bool live = true;
        while (live) {
            bool more = true;
            while (more) {
                const std::string where = "lockstep hand " + std::to_string(hand)
                    + " decision " + std::to_string(decisions++);
                for (bool memoryBasic : {false, true}) {
                    trainer->setActionState(memoryBasic);
                    table->setActionState(memoryBasic);
                    if (trainer->getActionState(memoryBasic) != table->getActionState(memoryBasic)) {
                        report(tally, where + ": action state differs");
                    }
                }
                const std::array<bool, 5> legal = trainer->getOptions();
                if (table->getOptions() != legal) {
                    report(tally, where + ": options differ");
                }
                tally.nodes++;
                tally.perRound[trainer->round]++;

                const auto groups = actingGroups(*trainer, metasBasic);
                if (actingGroups(*table, metasBasic) != groups) {
                    report(tally, where + ": acting groups differ");
                }
                // A random policy per group over the legal options, with some options
                // near zero so the floor bites.
                std::array<std::array<double, 5>, GROUP_COUNT> policy = {};
                for (auto& row : policy) {
                    double sum = 0;
                    for (int j = 0; j < 5; j++) {
                        row[j] = legal[j] ? std::pow(unit(gen), 6.0) : 0.0;
                        sum += row[j];
                    }
                    for (double& p : row) {
                        p = sum > 0 ? p / sum : 0.0;
                    }
                }
                std::vector<int> choices;
                for (int option = 0; option < 5; option++) {
                    if (legal[option]) {
                        choices.push_back(option);
                        if (option == 1) {
                            choices.insert(choices.end(), 4, 1);
                        }
                    }
                }
                const int option = choices[std::uniform_int_distribution<int>(
                    0, int(choices.size()) - 1)(gen)];
                const int seat = trainer->action;
                trainerNarrow(*trainer, seat, groups, policy, option);
                table->applyPolicy(seat, groups, policy, option);

                more = trainer->play(option);
                if (table->play(option) != more) {
                    report(tally, where + ": play() returned differently");
                }
                compareSeats(where);
            }
            const bool next = trainer->beginNewRound();
            if (table->beginNewRound() != next) {
                report(tally, "lockstep hand " + std::to_string(hand) + ": beginNewRound differs");
            }
            if (!next) {
                break;
            }
            trainer->setCommonRange();
            table->setCommonRange();
            if (trainer->getCommonRange() != table->commonRange) {
                report(tally, "lockstep hand " + std::to_string(hand) + ": common range differs");
            }
            // The trainer drops a hand whose board took some seat's whole range.
            if (!trainer->rangesLive()) {
                break;
            }
            trainer->setMetaState(true, true);
            table->setMetaState(true, true);
            for (int combo = 0; combo < COMBO_COUNT; combo++) {
                bool classified = trainer->commonRange[combo];
                bool anyLive = false;
                for (int seat = 0; seat < PLAYER_COUNT && classified; seat++) {
                    anyLive |= trainer->players[seat].inHand && trainer->players[seat].range[combo] > 0.0f;
                }
                if (classified && anyLive && (trainer->metaState[combo] != table->metaState[combo]
                        || trainer->metaStateBasic[combo] != table->metaStateBasic[combo])) {
                    report(tally, "lockstep hand " + std::to_string(hand) + ": meta state differs at combo "
                        + std::to_string(combo));
                    break;
                }
            }
        }
    }
}

template <int P>
static void run(uint64_t hands, Tally& tally) {
    std::mt19937 gen(15 + P);
    auto engine = std::make_unique<TableEngine<P>>();
    auto replay = std::make_unique<Replay<P>>();
    std::uniform_int_distribution<int> coin(0, 1);
    // A wider chip range than training uses, so short stacks reach the all-in branches.
    std::uniform_int_distribution<int> stack(5, 200);
    std::array<int, 52> deck;
    std::iota(deck.begin(), deck.end(), 0);

    for (uint64_t hand = 0; hand < hands; hand++) {
        StrategyConfig config;
        config.players = P;
        config.chips = 2 * stack(gen);
        config.metasBasic = coin(gen);
        config.memoryBasic = coin(gen);
        for (int i = 0; i < 5; i++) {
            std::swap(deck[i], deck[std::uniform_int_distribution<int>(i, 51)(gen)]);
        }
        engine->startingChips = float(config.chips);
        engine->deal({deck[0], deck[1], deck[2], deck[3], deck[4]});
        std::vector<int> actions;
        while (true) {
            bool more = true;
            while (more) {
                engine->setActionState(false);
                engine->setActionState(true);
                const std::array<bool, 5> legal = engine->getOptions();

                std::vector<int> board(engine->boardCards.begin(),
                    engine->boardCards.begin() + roundBoardCards(engine->round));
                const std::string where = "P=" + std::to_string(P) + " hand " + std::to_string(hand)
                    + " round " + std::to_string(engine->round) + " after "
                    + std::to_string(actions.size()) + " actions";
                ReplayResult result = replay->run(config, actions, board, nullptr);
                tally.nodes++;
                tally.perRound[engine->round]++;
                if (!result.ok) {
                    report(tally, where + ": replay failed: " + result.error);
                } else {
                    if (replay->engine.getActionState(config.memoryBasic)
                            != engine->getActionState(config.memoryBasic)) {
                        report(tally, where + ": action state differs");
                    }
                    if (result.legal != legal) {
                        report(tally, where + ": legal set differs");
                    }
                    const auto& mine = replay->engine.getMetaState(config.metasBasic);
                    const auto& theirs = engine->getMetaState(config.metasBasic);
                    for (int combo = 0; combo < COMBO_COUNT; combo++) {
                        if (engine->commonRange[combo] != replay->engine.commonRange[combo]) {
                            report(tally, where + ": common range differs");
                            break;
                        }
                        if (engine->commonRange[combo] && mine[combo] != theirs[combo]) {
                            report(tally, where + ": meta state differs at combo " + std::to_string(combo));
                            break;
                        }
                    }
                    checkDecode(*replay, tally, where);
                }
                for (int option = 0; option < 5; option++) {
                    const bool explained = !illegalReason(*engine, option).empty();
                    if (explained == legal[option]) {
                        report(tally, where + ": option " + std::to_string(option)
                            + (legal[option] ? " is legal but has a reason" : " is illegal with no reason"));
                    }
                }

                std::vector<int> choices;
                for (int option = 0; option < 5; option++) {
                    if (legal[option]) {
                        choices.push_back(option);
                        if (option == 1) {
                            choices.insert(choices.end(), 4, 1);
                        }
                    }
                }
                const int option = choices[std::uniform_int_distribution<int>(
                    0, int(choices.size()) - 1)(gen)];
                const int seat = engine->action;
                const float promised = engine->betTo(option);
                actions.push_back(option);
                more = engine->play(option);
                if (option >= 2 && engine->players[seat].bet != promised) {
                    report(tally, where + ": raise landed on " + std::to_string(engine->players[seat].bet)
                        + ", betTo promised " + std::to_string(promised));
                }
            }
            if (!engine->beginNewRound()) {
                break;
            }
            engine->setCommonRange();
            engine->setMetaState(true, true);
        }
    }
}

int main(int argc, char** argv) {
    try {
        const uint64_t hands = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
        Engine::loadTables();
        ServerTables::load();

        Tally lock;
        lockstep(hands, lock);
        std::cout << "Lockstep with Engine (" << PLAYER_COUNT << " seats, " << CHIP_COUNT
                  << " chips): " << lock.nodes << " decisions\n"
                  << "  preflop " << lock.perRound[0] << ", flop " << lock.perRound[1]
                  << ", turn " << lock.perRound[2] << ", river " << lock.perRound[3] << "\n"
                  << "  failures: " << lock.failures << std::endl;

        Tally tally;
        run<3>(hands, tally);
        run<4>(hands, tally);
        run<5>(hands, tally);
        run<6>(hands, tally);
        run<7>(hands, tally);
        std::cout << "Replay and key decode, 3..7 seats: " << tally.nodes << " nodes\n"
                  << "  preflop " << tally.perRound[0] << ", flop " << tally.perRound[1]
                  << ", turn " << tally.perRound[2] << ", river " << tally.perRound[3] << "\n"
                  << "  failures: " << tally.failures << "\n"
                  << (lock.failures + tally.failures == 0 ? "PASSED" : "FAILED") << std::endl;
        return lock.failures + tally.failures == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
