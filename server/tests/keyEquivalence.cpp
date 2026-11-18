// Proves the server reconstructs exactly what the trainer's engine holds.
//
//   build/keyEquivalence [hands]
//
// Part 1, lockstep. The trainer's own Engine (solver/trainer: PLAYER_COUNT seats,
// CHIP_COUNT chips, ANTE) and the server's TableEngine<PLAYER_COUNT> play the same hands
// side by side, the way Explorer::traverse plays them: setActionState / getOptions /
// play / beginNewRound / setMetaState. The two must agree on every memory stage's
// action state, the options, every seat's chips and
// bets, the pot, what play() and beginNewRound() return, and, for each seat's own two
// cards, the meta state of every metas stage whose tables are built. When the default
// strategy file (strategy_<config.h's spec>.bin) exists, the hands follow its policy at
// INV_TEMP - as self-play does - and the share of decisions whose key the file holds is
// reported: a server key that drifted from the trainer's would find almost nothing.
//
// Part 2, every table size the server dispatches on (3..7). TableEngine self plays
// under random stages, stacks and antes, calls weighted up so the walk reaches the river,
// and at every decision:
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
#include <map>

struct Tally {
    uint64_t nodes = 0;
    std::array<uint64_t, 4> perRound = {};
    std::array<uint64_t, 4> found = {};
    uint64_t failures = 0;
};

static void report(Tally& tally, const std::string& what) {
    tally.failures++;
    if (tally.failures <= 20) {
        std::cerr << what << std::endl;
    }
}

// The SPR / CPR bucket setActionState() writes, worked out independently.
static int expectedSprCpr(float spr, float cpr) {
    if (spr < 0.0005f) {
        for (int i = 0; i < 3; i++) {
            if (cpr < CPR_THRESHOLDS[i]) {
                return 6 + i;
            }
        }
        return 9;
    }
    for (int i = 0; i < 5; i++) {
        if (spr < SPR_THRESHOLDS[i]) {
            return i;
        }
    }
    return 5;
}

static int bucket(int x) {
    return (x > 0) + (x > 2);
}

// The metas stages whose tables are built. Stage C needs handMetasC.bin and
// preflopMetas.bin, which a checkout may not have yet.
static uint32_t loadStages() {
    uint32_t mask = 0;
    for (int stage = 0; stage < METAS_STAGE_COUNT; stage++) {
        try {
            ServerTables::requireStage(stage);
            mask |= 1u << stage;
        } catch (const std::exception& e) {
            std::cout << "Metas stage " << char('A' + stage) << " skipped: " << e.what() << "\n";
        }
    }
    return mask;
}

// Engine::loadTables would load handScores.bin (only showdown reads it) and fail on any
// stage whose tables are not built, so the trainer's statics are copied from the
// server's. A stage that is not loaded gets stage A's board classes: setMetaState reads
// every stage's board class, but keys only the stages in its mask.
static void seedTrainerTables(uint32_t stageMask) {
    Engine::binomials = ServerTables::binomials;
    Engine::preflopHandClasses = ServerTables::preflopHandClasses;
    Engine::preflopClusters = ServerTables::preflopClusters;
    const int fallback = std::countr_zero(stageMask);
    for (int stage = 0; stage < METAS_STAGE_COUNT; stage++) {
        const int from = stageMask >> stage & 1 ? stage : fallback;
        Engine::boardClassTables[stage] = ServerTables::boardClassTables[from];
        Engine::handMetas[stage] = ServerTables::handMetas[from];
    }
}

template <int P>
static void checkDecode(const Replay<P>& replay, Tally& tally, const std::string& where) {
    const KeyLayout layout = decodeKey(replay);
    const auto& engine = replay.engine;
    const auto& hero = engine.players[engine.action];
    const auto& villan = engine.players[hero.villanPosition];
    const int round = engine.round;
    std::map<std::string, const KeyField*> byName;
    uint64_t repacked = 0;
    for (const KeyField& field : layout.fields) {
        byName[field.name] = &field;
        repacked |= field.value << field.shift;
    }
    if (repacked != layout.sharedKey) {
        report(tally, where + ": fields do not repack into the shared key");
    }
    if ((layout.sharedKey & ((1ULL << ACTION_STATE_BITS) - 1))
            != engine.getActionState(replay.config.memoryStage)) {
        report(tally, where + ": shared key does not hold the action state");
    }
    auto expect = [&](const std::string& name, int got, int want) {
        if (got != want) {
            report(tally, where + ": " + name + " decoded " + std::to_string(got)
                + ", engine holds " + std::to_string(want));
        }
    };
    auto field = [&](const char* name) -> const KeyField& {
        auto at = byName.find(name);
        if (at == byName.end()) {
            throw std::runtime_error(std::string("decodeKey has no field ") + name);
        }
        return *at->second;
    };
    expect("headsBasic", int(field("headsBasic").value), engine.headsBasic);
    expect("preflopHeadsBasic", int(field("preflopHeadsBasic").value), engine.preflopHeadsBasic & 1);
    expect("sprCpr", int(field("sprCpr").value), expectedSprCpr(hero.spr, hero.cpr));
    for (int street = 0; street < 4; street++) {
        expect("raiseHistory", field("raiseHistory").parts[street].value, engine.raiseHistory[street]);
        expect("raiseSizes", field("raiseSizes").parts[street].value, engine.raiseSizeHistory[street] > 0);
    }

    // The story is the trainer's base 3 / base 5 number, masked to 11 bits. A past
    // street where hero and villain both raised last writes a digit of 3, which carries,
    // so the digits are only compared when every one fits.
    std::array<int, 4> digits;
    uint64_t story = 0;
    bool fits = true;
    for (int street = 0; street < 4; street++) {
        digits[street] = street != round
            ? 2 * hero.lastActions[street] + villan.lastActions[street]
            : hero.lastActions[street] + villan.lastActions[street];
        fits &= digits[street] <= 2;
        story = story * 3 + digits[street];
    }
    story = (story * 5 + hero.positionBasic) * 5 + villan.positionBasic;
    fits &= story <= 0x7FF;
    expect("story", int(field("story").value), int(story & 0x7FF));
    if (fits) {
        for (int street = 0; street < 4; street++) {
            expect("story digit", field("story").parts[street].value, digits[street]);
        }
        expect("hero position", field("story").parts[4].value, hero.positionBasic);
        expect("villain position", field("story").parts[5].value, villan.positionBasic);
    }

    const bool raised = engine.raiseHistory[round] > 0;
    const KeyField& lastRaise = field("lastRaise");
    expect("lastRaise faced", lastRaise.parts[0].value, raised * std::min<int>(engine.raiseStrength, 2));
    expect("lastRaise behind", lastRaise.parts[1].value, raised * bucket(engine.raiseActionPosition));
    expect("lastRaise callers", lastRaise.parts[2].value, raised * std::min<int>(engine.hiddenCalls, 2));
    if (lastRaise.zeroed != !raised) {
        report(tally, where + ": lastRaise zeroed flag is wrong");
    }
    expect("hiddenRaiseSize", int(field("hiddenRaiseSize").value),
        (engine.raiseHistory[round] > 1) * (engine.hiddenRaiseSize > 0));
    const KeyField& spot = field("hero");
    expect("hero behind", spot.parts[0].value, bucket(engine.actionPosition));
    expect("hero raises faced", spot.parts[1].value, std::min<int>(hero.raisesFaced, 2));
    expect("hero callers", spot.parts[2].value, std::min<int>(engine.calls, 2));
    expect("relativePosition", int(field("relativePosition").value), hero.relativePosition);
    expect("calledTwice", int(field("calledTwice").value), hero.calls > 1);
    if (round > 0) {
        expect("boardField", int(field("boardField").value),
            engine.boardClass[replay.config.metasStage] + 169);
    }
}

// The options a random walk picks from: each legal option once, calls five times, so
// hands reach the flop rather than ending preflop on a fold or a raise war.
static int pickOption(const std::array<bool, 5>& legal, std::mt19937& gen) {
    std::vector<int> choices;
    for (int option = 0; option < 5; option++) {
        if (legal[option]) {
            choices.push_back(option);
            if (option == 1) {
                choices.insert(choices.end(), 4, 1);
            }
        }
    }
    return choices[std::uniform_int_distribution<int>(0, int(choices.size()) - 1)(gen)];
}

// Draws from a record's policy at INV_TEMP, as Explorer::drawOption does.
static int drawOption(const StrategyRecord& record, const std::array<bool, 5>& legal,
        std::mt19937& gen) {
    const std::array<double, 5> policy = policyFromWeights(record.weights, legal, INV_TEMP);
    double sample = std::uniform_real_distribution<double>(0.0, 1.0)(gen);
    int option = 0;
    for (int j = 0; j < 5; j++) {
        if (policy[j] > 0) {
            option = j;
            if (sample < policy[j]) {
                break;
            }
            sample -= policy[j];
        }
    }
    return option;
}

static void lockstep(uint64_t hands, uint32_t stageMask, const StrategyStore* store, Tally& tally) {
    std::mt19937 gen(7);
    auto trainer = std::make_unique<Engine>();
    auto table = std::make_unique<TableEngine<PLAYER_COUNT>>();
    table->startingChips = CHIP_COUNT;
    table->ante = ANTE;
    table->metasStageMask = stageMask;

    auto compare = [&](const std::string& where) {
        for (int seat = 0; seat < PLAYER_COUNT; seat++) {
            const auto& a = trainer->players[seat];
            const auto& b = table->players[seat];
            if (a.chips != b.chips || a.bet != b.bet || a.inHand != b.inHand
                    || a.villanPosition != b.villanPosition || a.calls != b.calls) {
                report(tally, where + ": seat " + std::to_string(seat)
                    + " chips, bet, inHand, villain or calls differ");
            }
        }
        if (trainer->pot != table->pot || trainer->bet != table->bet || trainer->round != table->round
                || trainer->action != table->action || trainer->heads != table->heads) {
            report(tally, where + ": pot, bet, round, action or heads differ");
        }
    };
    auto compareMetas = [&](const std::string& where) {
        for (int seat = 0; seat < PLAYER_COUNT; seat++) {
            const auto& player = trainer->players[seat];
            if (!player.inHand) {
                continue;
            }
            const int combo = holePairIndex(player.holeCards[0], player.holeCards[1]);
            for (int stage = 0; stage < METAS_STAGE_COUNT; stage++) {
                if ((stageMask >> stage & 1) && player.metaState[stage] != table->metaState[stage][combo]) {
                    report(tally, where + ": stage " + char('A' + stage) + " meta differs for seat "
                        + std::to_string(seat));
                }
            }
        }
    };

    for (uint64_t hand = 0; hand < hands; hand++) {
        trainer->shuffle(gen);
        trainer->deal();
        table->deal({trainer->deck[0], trainer->deck[1], trainer->deck[2],
            trainer->deck[3], trainer->deck[4]});
        compareMetas("lockstep hand " + std::to_string(hand) + " deal");
        int decisions = 0;
        while (true) {
            const std::string where = "lockstep hand " + std::to_string(hand)
                + " step " + std::to_string(decisions++);
            int option;
            {
                for (int stage = 0; stage < MEMORY_STAGE_COUNT; stage++) {
                    trainer->setActionState(stage);
                    table->setActionState(stage);
                    if (trainer->getActionState(stage) != table->getActionState(stage)) {
                        report(tally, where + ": memory stage " + std::to_string(stage)
                            + " action state differs");
                    }
                }
                const std::array<bool, 5> legal = trainer->getOptions();
                if (table->getOptions() != legal) {
                    report(tally, where + ": options differ");
                }
                tally.nodes++;
                tally.perRound[trainer->round]++;
                StrategyRecord record;
                const uint64_t key = trainer->getActionState(MEMORY_STAGE)
                    | trainer->getMetaState(METAS_STAGE) << ACTION_STATE_BITS;
                if (store != nullptr && store->lookup(key, record)) {
                    tally.found[trainer->round]++;
                    option = drawOption(record, legal, gen);
                } else {
                    option = pickOption(legal, gen);
                }
            }
            const bool more = trainer->play(option);
            if (table->play(option) != more) {
                report(tally, where + ": play() returned differently");
            }
            compare(where);
            if (more) {
                continue;
            }
            const bool next = trainer->beginNewRound();
            if (table->beginNewRound() != next) {
                report(tally, where + ": beginNewRound differs");
            }
            if (!next) {
                break;
            }
            trainer->setMetaState(stageMask);
            table->setCommonRange();
            table->setMetaState();
            compareMetas(where + " new street");
        }
    }
}

template <int P>
static void run(uint64_t hands, uint32_t stageMask, Tally& tally) {
    std::mt19937 gen(15 + P);
    auto engine = std::make_unique<TableEngine<P>>();
    auto replay = std::make_unique<Replay<P>>();
    // Stacks across the whole range a spec can name, so short stacks reach the all-in
    // branches, and every ante.
    std::uniform_int_distribution<int> stack(MIN_CHIPS / CHIPS_STEP, 60);
    std::uniform_int_distribution<int> anteDist(0, MAX_ANTE);
    std::uniform_int_distribution<int> memoryDist(0, MEMORY_STAGE_COUNT - 1);
    std::vector<int> stages;
    for (int stage = 0; stage < METAS_STAGE_COUNT; stage++) {
        if (stageMask >> stage & 1) {
            stages.push_back(stage);
        }
    }
    std::array<int, 52> deck;
    std::iota(deck.begin(), deck.end(), 0);

    for (uint64_t hand = 0; hand < hands; hand++) {
        StrategyConfig config;
        config.players = P;
        config.chips = CHIPS_STEP * stack(gen);
        config.ante = anteDist(gen);
        config.metasStage = stages[std::uniform_int_distribution<int>(0, int(stages.size()) - 1)(gen)];
        config.memoryStage = memoryDist(gen);
        for (int i = 0; i < 5; i++) {
            std::swap(deck[i], deck[std::uniform_int_distribution<int>(i, 51)(gen)]);
        }
        engine->startingChips = float(config.chips);
        engine->ante = float(config.ante);
        engine->metasStageMask = 1u << config.metasStage;
        engine->deal({deck[0], deck[1], deck[2], deck[3], deck[4]});
        std::vector<int> actions;
        while (true) {
            bool more = true;
            {
                engine->setActionState(config.memoryStage);
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
                    if (replay->engine.getActionState(config.memoryStage)
                            != engine->getActionState(config.memoryStage)) {
                        report(tally, where + ": action state differs");
                    }
                    if (result.legal != legal || replay->engine.action != engine->action) {
                        report(tally, where + ": legal set or acting seat differs");
                    }
                    const auto& mine = replay->engine.getMetaState(config.metasStage);
                    const auto& theirs = engine->getMetaState(config.metasStage);
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

                const int option = pickOption(legal, gen);
                const int seat = engine->action;
                const float promised = engine->betTo(option);
                actions.push_back(option);
                more = engine->play(option);
                if (option >= 2 && engine->players[seat].bet != promised) {
                    report(tally, where + ": raise landed on " + std::to_string(engine->players[seat].bet)
                        + ", betTo promised " + std::to_string(promised));
                }
            }
            if (more) {
                continue;
            }
            if (!engine->beginNewRound()) {
                break;
            }
            engine->setCommonRange();
            engine->setMetaState();
        }
    }
}

int main(int argc, char** argv) {
    try {
        const uint64_t hands = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 4000;
        ServerTables::load();
        const uint32_t stageMask = loadStages();
        if (stageMask == 0) {
            throw std::runtime_error("no metas stage has its tables built");
        }
        seedTrainerTables(stageMask);

        // The default strategy file, when there is one and its metas stage is built.
        std::unique_ptr<StrategyStore> store;
        const std::string spec = StrategyConfig{}.spec();
        if ((stageMask >> METAS_STAGE & 1) && std::filesystem::exists(strategyPath(spec))) {
            const std::string index = indexPathFor(strategyPath(spec));
            store = std::make_unique<StrategyStore>();
            try {
                store->open(strategyPath(spec), index);
            } catch (const std::exception& e) {
                std::cout << "Index unusable (" << e.what() << "), rebuilding" << std::endl;
                buildIndex(strategyPath(spec), index, &std::cout);
                store->open(strategyPath(spec), index);
            }
        }

        Tally lock;
        lockstep(hands, stageMask, store.get(), lock);
        std::cout << "Lockstep with Engine (" << PLAYER_COUNT << " seats, " << CHIP_COUNT
                  << " chips, ante " << ANTE << "): " << lock.nodes << " decisions, "
                  << "\n  preflop " << lock.perRound[0] << ", flop " << lock.perRound[1]
                  << ", turn " << lock.perRound[2] << ", river " << lock.perRound[3] << "\n";
        if (store != nullptr) {
            std::cout << std::fixed << std::setprecision(2) << "  strategy_" << spec
                      << ".bin holds the key at";
            for (int street = 0; street < 4; street++) {
                std::cout << (street ? ", " : " ") << 100.0 * double(lock.found[street])
                    / double(std::max<uint64_t>(lock.perRound[street], 1)) << "% "
                    << STREET_NAMES[street];
            }
            std::cout << " (play follows its policy where it does)\n";
        } else {
            std::cout << "  no strategy_" << spec << ".bin: random play, no coverage check\n";
        }
        std::cout << "  failures: " << lock.failures << std::endl;

        Tally tally;
        run<3>(hands, stageMask, tally);
        run<4>(hands, stageMask, tally);
        run<5>(hands, stageMask, tally);
        run<6>(hands, stageMask, tally);
        run<7>(hands, stageMask, tally);
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
