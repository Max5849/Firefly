#include "solver/multiHand/config.h"
#include "solver/multiHand/engine.h"
#include "solver/multiHand/strategyTable.h"
#include "solver/multiHand/baseTable.h"
#include "solver/multiHand/trace.h"

double MIN_DELTA_INC;
double DELTA_SCALAR;
double INV_MIN_DELTA_INC;
double INV_DELTA_SCALAR;

// Applies w <- scale * w + shift atomically. A run of EMA steps w <- w + s (t - w)
// composes into one such map, so a node's steps from one decision land in one CAS.
static inline std::pair<double, double> affineUpdate(std::atomic<uint64_t>& word,
            double scale, double shift) {
    uint64_t old = word.load(std::memory_order_relaxed);
    uint64_t next;
    double value;
    do {
        value = std::bit_cast<double>(old);
        next = std::bit_cast<uint64_t>(scale * value + shift);
    } while (!word.compare_exchange_weak(old, next,
        std::memory_order_relaxed, std::memory_order_relaxed));
    return {value, std::bit_cast<double>(next)};
}

// A combo's node within a decision; see Engine::getGroup. NO_GROUP marks a combo that
// was out of the range.
inline constexpr int GROUP_COUNT = 256;
inline constexpr uint8_t NO_GROUP = 0xFF;

// One decision of the hand. Every combo of the acting seat still in its range was
// assigned to the node its key names; each node was looked up once.
struct RangeDecision {
    int seat;
    int option;
    float chips;
    float pot;
    // The line's odds and every combo's line reach (Engine::lineReach) just before the
    // decision; reach / odds is the expected weight of a combo's touch given the hand
    // reached it.
    double odds;
    std::array<double, COMBO_COUNT> reach;
    int groupCount;
    std::array<uint8_t, GROUP_COUNT> liveGroups;
    std::array<Entry*, GROUP_COUNT> entries;
    // Each node's INV_TEMP probability for the option taken, divided back out of its
    // combos' touches so the update is conditioned on taking the option.
    std::array<double, GROUP_COUNT> share;
    std::array<uint8_t, COMBO_COUNT> groups;
};

struct Worker {
    Engine engine;
    std::mt19937 gen;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    // Reused across hands; decisionCount is how many are live in this one.
    std::vector<RangeDecision> decisions;
    size_t decisionCount = 0;
    // Per node scratch, indexed by group.
    std::array<uint64_t, GROUP_COUNT> keys;
    std::array<int, GROUP_COUNT> firstCombo;
    std::array<double, GROUP_COUNT> groupReach;
    std::array<std::array<double, 5>, GROUP_COUNT> policy;
    std::array<std::array<double, 5>, GROUP_COUNT> sampling;
    std::array<double, GROUP_COUNT> visitMass;
    std::array<double, GROUP_COUNT> step;
    std::array<double, GROUP_COUNT> scale;
    std::array<double, GROUP_COUNT> shift;
    // Per seat scratch for updateRangeWeights: each combo's final line reach, filled the
    // first time one of the seat's decisions trains.
    std::array<std::array<double, COMBO_COUNT>, PLAYER_COUNT> lineWeight;
    std::array<bool, PLAYER_COUNT> lineWeighed;
    std::vector<TraceNode> tracePath;

    explicit Worker(unsigned t) : gen(15 + t) {
        decisions.reserve(64);
    }
};

enum class Phase { Warmup, Training, Cooldown };

struct Explorer {
    static constexpr uint64_t BLOCK = EPOCHS / 20;
    static constexpr uint64_t BLOCKS = EPOCHS / BLOCK;
    static constexpr uint64_t WARMUP_HANDS = EPOCHS / 20;
    static constexpr uint64_t COOLDOWN_HANDS = EPOCHS / 20;
    static_assert(BLOCK > 0 && EPOCHS % BLOCK == 0,
        "BLOCK must divide EPOCHS exactly");
    static_assert(BLOCKS >= 1, "the run needs at least one training block");

    StrategyTable strategy;
    BaseTable strategyBase;
    unsigned threads;
    std::barrier<> bar;
    std::atomic<uint64_t> nextChunk;
    // Chunks of the current phase block that have finished, as opposed to been claimed.
    std::atomic<uint64_t> doneChunks;
    // Hands finished over the whole run, across every phase. On its own cache line, since
    // every thread bumps it once a hand (128 bytes covers Apple silicon's line size).
    alignas(128) std::atomic<uint64_t> totalHands;
    alignas(128) std::mutex printMutex;
    // The hand count and time at the last report, guarded by printMutex.
    uint64_t lastReportHands;
    std::chrono::steady_clock::time_point lastReportTime;

    explicit Explorer(unsigned threads) : threads(threads), bar(threads), nextChunk(0), doneChunks(0),
        totalHands(0), lastReportHands(0), lastReportTime(std::chrono::steady_clock::now()) {}

    // Finds the node for one key, inserting it if new. combo is a combo that reaches the
    // key, used only for the base key. Visits are not counted here: they are counted at
    // the end of the hand, weighted by reach.
    Entry& locateNode(Engine& engine, int combo, uint64_t key, const std::array<bool, 5>& options) {
        bool inserted = false;
        uint64_t slotIndex = 0;
        uint64_t word = 0;
        Entry& weights = *strategy.locate(key, slotIndex, word, inserted);
        if (inserted) {
            const BaseEntry* base = nullptr;
            if constexpr (USE_BASE) {
                base = strategyBase.find(engine.getActionState(BASE_MEMORY_BASIC)
                    | engine.getMetaState(BASE_METAS_BASIC)[combo] << 42);
            }
            uint8_t legal = 0;
            if (base == nullptr) {
                for (int j = 0; j < 5; j++) {
                    if (options[j]) {
                        legal |= uint8_t(1u << j);
                    }
                }
            } else {
                std::array<double, 5> draw = {0};
                double sum = 0;
                if (options[0]) {
                    draw[0] = 1.0;
                }
                for (int j = 1; j < 5; j++) {
                    if (options[j]) {
                        draw[j] = std::exp(std::clamp(
                            INV_TEMP * double(base->weights[j - 1]),
                            -MAX_EXP_ARG, MAX_EXP_ARG));
                    }
                }
                for (int j = 0; j < 5; j++) {
                    sum += draw[j];
                }
                for (int j = 0; j < 5; j++) {
                    if (options[j] && BASE_CONFIDENCE * draw[j] / sum >= 1.0) {
                        legal |= uint8_t(1u << j);
                    }
                }
            }
            for (int j = 0; j < 4; j++) {
                double seed = base != nullptr ? double(base->weights[j]) : 0.0;
                weights.weights[j].store(std::bit_cast<uint64_t>(seed),
                    std::memory_order_relaxed);
            }
            weights.stats.store(makeStats(legal, 65535), std::memory_order_relaxed);
            strategy.publish(slotIndex, word);
        }
        return weights;
    }

    // The node's policy at INV_TEMP, and its sampling policy at SAMPLE_TEMP, over its
    // legal options. Fold's weight is the exact 0, so it draws exp(0) = 1 in both.
    static void nodePolicy(const Entry& weights, std::array<double, 5>& probabilities,
            std::array<double, 5>& sampling) {
        const uint8_t legal = entryOptions(weights.stats.load(std::memory_order_relaxed));
        double sum = 0;
        double sampleSum = 0;
        for (int j = 0; j < 5; j++) {
            double draw = 0;
            double sampleDraw = 0;
            if (legal & (1u << j)) {
                double w = j == 0 ? 0.0 : std::bit_cast<double>(
                    weights.weights[j - 1].load(std::memory_order_relaxed));
                draw = std::exp(std::clamp(INV_TEMP * w, -MAX_EXP_ARG, MAX_EXP_ARG));
                sampleDraw = std::exp(std::clamp(SAMPLE_INV_TEMP * w, -MAX_EXP_ARG, MAX_EXP_ARG));
            }
            probabilities[j] = draw;
            sum += draw;
            sampling[j] = sampleDraw;
            sampleSum += sampleDraw;
        }
        for (int j = 0; j < 5; j++) {
            probabilities[j] /= sum;
            sampling[j] /= sampleSum;
        }
    }

    // Samples the acting seat's action from its whole range. Combos sharing a key share
    // a node, so each node is looked up once and its sampling policy weighted by the
    // summed reach of its combos; one option is drawn from the sum. Every combo's reach
    // is then multiplied by the probability its node's INV_TEMP policy gave that option,
    // and dropped to 0 below RANGE_FLOOR times the range's summed reach.
    //
    // The line's odds take the option's sampled share. What the INV_TEMP policies make of
    // the action is not a scalar: it lives in the ranges, and Engine::lineReach reads it
    // per combo with each combo's card removal, so a combo that blocks the hands an
    // opponent continues with sees that opponent fold more often.
    int chooseRangeOption(Worker& worker) {
        Engine& engine = worker.engine;
        const int seat = engine.getAction();
        std::array<float, COMBO_COUNT>& range = engine.getRange(seat);
        const std::array<bool, COMBO_COUNT>& commonRange = engine.getCommonRange();
        // setActionState() wrote spr and cpr, which getOptions() reads; neither depends
        // on the combo, so legality is shared by the whole range.
        const std::array<bool, 5> options = engine.getOptions();
        const uint64_t actionState = engine.getActionState(MEMORY_BASIC);
        const std::array<uint64_t, COMBO_COUNT>& metaState = engine.getMetaState(METAS_BASIC);

        if (worker.decisionCount == worker.decisions.size()) {
            worker.decisions.emplace_back();
        }
        const int decisionIndex = int(worker.decisionCount++);
        RangeDecision& decision = worker.decisions[decisionIndex];
        decision.seat = seat;
        decision.chips = engine.getChips();
        decision.pot = engine.getPot();
        decision.odds = engine.lineOdds;
        decision.groupCount = 0;
        decision.groups.fill(NO_GROUP);
        worker.firstCombo.fill(-1);

        // Keys first, one per node, prefetching each home slot so the lookups below
        // overlap their cache misses.
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (!commonRange[combo] || !(range[combo] > 0.0f)) {
                continue;
            }
            const uint8_t group = engine.getGroup(METAS_BASIC, combo);
            if (worker.firstCombo[group] < 0) {
                worker.firstCombo[group] = combo;
                worker.groupReach[group] = 0.0;
                decision.liveGroups[decision.groupCount++] = group;
                uint64_t key = actionState | metaState[combo] << 42;
                worker.keys[group] = key;
                strategy.prefetch(key);
            }
            worker.groupReach[group] += range[combo];
            decision.groups[combo] = group;
        }
        // Every combo's line reach, read only for combos with a group. Filled here, while
        // the prefetches above are in flight.
        engine.lineReach(seat, decision.reach);

        std::array<double, 5> aggregate = {};
        std::array<double, 5> sampled = {};
        for (int k = 0; k < decision.groupCount; k++) {
            const uint8_t group = decision.liveGroups[k];
            Entry& weights = locateNode(engine, worker.firstCombo[group], worker.keys[group], options);
            decision.entries[group] = &weights;
            nodePolicy(weights, worker.policy[group], worker.sampling[group]);
            for (int j = 0; j < 5; j++) {
                aggregate[j] += worker.groupReach[group] * worker.policy[group][j];
                sampled[j] += worker.groupReach[group] * worker.sampling[group][j];
            }
        }

        double running = 0;
        std::array<double, 5> dividers;
        for (int j = 0; j < 5; j++) {
            running += sampled[j];
            dividers[j] = running;
        }
        if (!(running > 0)) {
            throw std::runtime_error("the acting seat's range is empty");
        }
        int option = 0;
        double sample = worker.unit(worker.gen) * running;
        for (int j = 0; j < 5; j++) {
            if (sample < dividers[j] && sampled[j] > 0) {
                option = j;
                break;
            }
        }
        decision.option = option;
        engine.lineOdds *= sampled[option] / running;
        for (int k = 0; k < decision.groupCount; k++) {
            const uint8_t group = decision.liveGroups[k];
            decision.share[group] = worker.policy[group][option];
        }

        if constexpr (TRACE_KEY_ENABLED) {
            for (int k = 0; k < decision.groupCount; k++) {
                const uint8_t group = decision.liveGroups[k];
                if (worker.keys[group] != TRACE_KEY) {
                    continue;
                }
                Entry& weights = *decision.entries[group];
                const uint64_t stats = weights.stats.load(std::memory_order_relaxed);
                TraceNode node{};
                node.key = worker.keys[group];
                node.decision = decisionIndex;
                node.group = group;
                node.combo = worker.firstCombo[group];
                node.action = seat;
                node.option = option;
                node.legal = entryOptions(stats);
                node.visits = entryVisits(stats);
                node.delta = entryDelta(stats);
                node.reach = worker.groupReach[group];
                for (int j = 0; j < 4; j++) {
                    node.weights[j] = std::bit_cast<double>(
                        weights.weights[j].load(std::memory_order_relaxed));
                }
                node.probabilities = worker.policy[group];
                for (int j = 0; j < 5; j++) {
                    node.aggregate[j] = aggregate[j] / running;
                    node.sampled[j] = sampled[j] / running;
                }
                node.chips = decision.chips;
                node.pot = decision.pot;
                node.updated = false;
                worker.tracePath.push_back(node);
            }
        }

        // The floor is relative to the range's summed reach, so the range can never be
        // emptied: an absolute floor drops a whole range whose reach has drifted down
        // over a long line.
        double rangeSum = 0;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            const uint8_t group = decision.groups[combo];
            if (group == NO_GROUP) {
                continue;
            }
            range[combo] *= float(worker.policy[group][option]);
            rangeSum += range[combo];
        }
        // The seat's card removal sums (Engine::lineReach) are rebuilt in the same pass. The
        // combos with a group are exactly those that miss the revealed board, which is what
        // blockMask holds between streets. Combos run in holePairIndex order, first card a
        // then second card b, so a's sum stays in a register and b's adds are contiguous.
        const double floor = RANGE_FLOOR * rangeSum;
        double blockTotal = 0;
        std::array<double, 52>& blockCard = engine.blockCard[seat];
        blockCard.fill(0.0);
        int combo = 0;
        for (int a = 0; a < 51; a++) {
            double cardA = 0;
            for (int b = a + 1; b < 52; b++, combo++) {
                const bool kept = decision.groups[combo] != NO_GROUP;
                if (kept && range[combo] < floor) {
                    range[combo] = 0.0f;
                }
                const double r = kept ? range[combo] : 0.0;
                cardA += r;
                blockCard[b] += r;
            }
            blockCard[a] += cardA;
            blockTotal += cardA;
        }
        engine.blockTotal[seat] = blockTotal;
        return option;
    }

    void printTrace(Worker& worker) {
        if (worker.tracePath.empty()) {
            return;
        }
        std::ostringstream out;
        formatTrace(worker.tracePath, out);
        worker.tracePath.clear();
        std::lock_guard<std::mutex> lock(printMutex);
        std::cout << out.str() << std::flush;
    }

    // At the end of the hand every combo still in its seat's range takes one EMA step
    // toward its own pot normalised return, scaled by its line reach over the line's
    // odds. The line was drawn at the sampling policies of the whole ranges, but the
    // combo plays it at its own reach, against the opponents' real frequencies given the
    // cards it and the board remove, so lineReach / lineOdds weights each touch by how
    // often it would really have happened. A node's steps from one decision are composed
    // and applied in one CAS, which equals applying them one combo at a time.
    //
    // The touch for a decision leaves out the node's own probability for the option: the
    // weight estimates the return given the option is taken. Left in, an option's weight
    // moved by about its probability's share of its gap per block, so an option the
    // policy had all but dropped could never climb back. Without it every option closes
    // about LEARNING_RATE of its gap per block, and a touch scales with 1 over the
    // option's sampled share instead, so its noise is set by how often the range samples
    // the option, not by how often the node plays it.
    //
    // Visits add each combo's expected weight given the hand reached the decision -
    // its line reach / odds just before it - rather than the weight itself: everything
    // from the decision on averages to 1, so the expectation drops that noise. Every
    // combo live at the decision counts, with no end of hand filter: lineReach's board
    // scale already pays back the combos a later board card rules out. In 1/256 units
    // with stochastic rounding.
    void updateRangeWeights(Worker& worker, double multiplier) {
        Engine& engine = worker.engine;
        // At a showdown a combo must also miss all five board cards: a hand that ends all
        // in before the river has not removed the turn and river from commonRange, and
        // setRewards gives a combo holding one of them no share of the pot. Otherwise the
        // revealed board, which the removal sums already hold, is the whole board. Weights
        // are only needed by a decision that trains, so warm-up and folds skip them.
        const bool showdown = engine.heads > 1;
        if (showdown && multiplier != 0.0 && engine.blockCount != 5) {
            engine.setBlocks(5);
        }
        worker.lineWeighed.fill(false);
        for (size_t d = 0; d < worker.decisionCount; d++) {
            const RangeDecision& decision = worker.decisions[d];
            const std::array<float, COMBO_COUNT>& reward = engine.getReward(decision.seat);
            const bool train = decision.option != 0 && multiplier != 0.0;
            std::array<double, COMBO_COUNT>& lineWeight = worker.lineWeight[decision.seat];
            if (train && !worker.lineWeighed[decision.seat]) {
                worker.lineWeighed[decision.seat] = true;
                engine.lineReach(decision.seat, lineWeight);
            }
            for (int k = 0; k < decision.groupCount; k++) {
                const uint8_t group = decision.liveGroups[k];
                worker.visitMass[group] = 0.0;
                worker.scale[group] = 1.0;
                worker.shift[group] = 0.0;
                worker.step[group] = train ? LEARNING_RATE * multiplier * MIN_DELTA_INC
                    * std::exp(DELTA_SCALAR * entryDelta(
                        decision.entries[group]->stats.load(std::memory_order_relaxed))) : 0.0;
            }
            for (int combo = 0; combo < COMBO_COUNT; combo++) {
                const uint8_t group = decision.groups[combo];
                if (group == NO_GROUP) {
                    continue;
                }
                worker.visitMass[group] += 256.0 * decision.reach[combo] / decision.odds;
                // A combo with reach left took the option with a share above 0: its float
                // reach would have underflowed to 0 otherwise.
                const double reach = train
                    ? lineWeight[combo] / (engine.lineOdds * decision.share[group]) : 0.0;
                if (reach > 0.0) {
                    // A touch of weight w is w unit steps: it keeps exp(-rate * w).
                    const double s = -std::expm1(-worker.step[group] * reach);
                    const double target = (reward[combo] - decision.chips) / decision.pot;
                    worker.scale[group] *= 1.0 - s;
                    worker.shift[group] = (1.0 - s) * worker.shift[group] + s * target;
                }
            }
            for (int k = 0; k < decision.groupCount; k++) {
                const uint8_t group = decision.liveGroups[k];
                Entry& weights = *decision.entries[group];
                const double mass = worker.visitMass[group];
                if (mass > 0.0) {
                    uint64_t add = uint64_t(mass);
                    add += worker.unit(worker.gen) < mass - double(add);
                    if (add > 0) {
                        addVisits(weights, add);
                    }
                }
                if (!train || worker.scale[group] == 1.0) {
                    continue;
                }
                auto [before, after] = affineUpdate(weights.weights[decision.option - 1],
                    worker.scale[group], worker.shift[group]);
                if constexpr (TRACE_KEY_ENABLED) {
                    for (TraceNode& node : worker.tracePath) {
                        if (node.decision != int(d) || node.group != group) {
                            continue;
                        }
                        node.updated = true;
                        node.step = 1.0 - worker.scale[group];
                        node.target = worker.shift[group] / node.step;
                        node.before = before;
                        node.after = after;
                        node.combos = 0;
                        node.targetMin = std::numeric_limits<double>::max();
                        node.targetMax = std::numeric_limits<double>::lowest();
                        for (int combo = 0; combo < COMBO_COUNT; combo++) {
                            if (decision.groups[combo] != group || !(lineWeight[combo] > 0.0)) {
                                continue;
                            }
                            double target = (reward[combo] - decision.chips) / decision.pot;
                            node.combos++;
                            node.targetMin = std::min(node.targetMin, target);
                            node.targetMax = std::max(node.targetMax, target);
                        }
                    }
                }
            }
            if constexpr (TRACE_KEY_ENABLED) {
                for (TraceNode& node : worker.tracePath) {
                    if (node.decision == int(d)) {
                        node.finalReach = worker.visitMass[node.group] / 256.0;
                    }
                }
            }
        }
        if constexpr (TRACE_KEY_ENABLED) {
            printTrace(worker);
        }
        worker.decisionCount = 0;
    }

    void updateDeltasRange(uint64_t begin, uint64_t end) {
        for (uint64_t i = begin; i < end; i++) {
            Entry& weights = strategy.dense[i];
            uint64_t stats = weights.stats.load(std::memory_order_relaxed);
            double visits = std::clamp(double(entryVisits(stats)), VISITS_FLOOR, VISITS_CEIL);
            double rate = 1.0 / std::pow(visits, FLEXIBILITY);
            long encoded = std::lround(std::log(rate * INV_MIN_DELTA_INC) * INV_DELTA_SCALAR);
            stats = (stats & ~DELTA_MASK) | uint64_t(std::clamp(encoded, 0L, 65535L)) << 40;
            weights.stats.store(stats, std::memory_order_relaxed);
        }
    }

    void deltaPass(unsigned t, bool keepVisits = false) {
        bar.arrive_and_wait();
        uint64_t live = strategy.count();
        uint64_t span = live / threads;
        uint64_t begin = span * t;
        uint64_t end = (t + 1 == threads) ? live : span * (t + 1);
        updateDeltasRange(begin, end);
        bar.arrive_and_wait();
        if (t == 0) {
            strategy.compact(keepVisits);
            nextChunk.store(0, std::memory_order_relaxed);
            doneChunks.store(0, std::memory_order_relaxed);
        }
        bar.arrive_and_wait();
    }

    void playHand(Worker& worker, double multiplier) {
        Engine& engine = worker.engine;
        engine.shuffle(worker.gen);
        engine.deal();
        worker.decisionCount = 0;
        while (true) {
            do {
                if (!MEMORY_BASIC || (USE_BASE && !BASE_MEMORY_BASIC)) {
                    engine.setActionState(false);
                }
                if (MEMORY_BASIC || (USE_BASE && BASE_MEMORY_BASIC)) {
                    engine.setActionState(true);
                }
            } while (engine.play(chooseRangeOption(worker)));
            if (!engine.beginNewRound()) {
                engine.setRewards();
                break;
            }
            engine.setCommonRange();
            if (!engine.rangesLive()) {
                // Every weight from here on is 0, so the hand is dropped. The decisions
                // already taken keep their visits, which are expectations at the decision.
                updateRangeWeights(worker, 0.0);
                return;
            }
            engine.setMetaState(!METAS_BASIC || (USE_BASE && !BASE_METAS_BASIC),
                METAS_BASIC || (USE_BASE && BASE_METAS_BASIC));
        }
        updateRangeWeights(worker, multiplier);
    }

    void runBlock(Worker& worker, Phase phase, uint64_t blockBase, uint64_t phaseHands) {
        const uint64_t phaseChunks = (phaseHands + CHUNK_HANDS - 1) / CHUNK_HANDS;
        for (;;) {
            uint64_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= phaseChunks) {
                return;
            }
            uint64_t base = chunk * CHUNK_HANDS;
            uint64_t hands = std::min(CHUNK_HANDS, phaseHands - base);
            double multiplier = (phase == Phase::Training) ? 1.0 : 0.0;
            for (uint64_t j = 0; j < hands; j++) {
                if (phase == Phase::Cooldown) {
                    multiplier = 1.0 - double(base + j) / double(phaseHands);
                }
                playHand(worker, multiplier);
                // Counted per hand, not per chunk, so a report's rate is not quantised to chunks.
                totalHands.fetch_add(1, std::memory_order_relaxed);
            }
            // Chunks finish out of order across threads, so progress is the count of
            // finished chunks, not the offset of the one this thread just played.
            uint64_t done = doneChunks.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done % PRINT_CHUNKS == 0 || done == phaseChunks) {
                report(phase, blockBase, phaseHands);
            }
        }
    }

    void report(Phase phase, uint64_t blockBase, uint64_t phaseHands) {
        // The count is read under the lock, so successive prints never go backwards.
        std::lock_guard<std::mutex> lock(printMutex);
        const uint64_t globalHand = blockBase + std::min(
            doneChunks.load(std::memory_order_relaxed) * CHUNK_HANDS, phaseHands);
        std::ostringstream out;
        out << std::fixed << std::setprecision(3);
        if (phase == Phase::Warmup) {
            out << "Warm up: " << 100.0 * double(globalHand) / double(WARMUP_HANDS) << "% Complete\n";
        } else if (phase == Phase::Training) {
            out << "Training: " << 100.0 * double(globalHand) / double(EPOCHS) << "% Complete\n";
        } else {
            out << "Cooldown: " << 100.0 * double(globalHand) / double(COOLDOWN_HANDS) << "% Complete\n";
        }
        out << "Strategy Size: " << strategy.count() << "\n";
        // The rate covers only the hands finished since the last report.
        const uint64_t hands = totalHands.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        const double hours = std::chrono::duration<double, std::ratio<3600>>(now - lastReportTime).count();
        if (hours > 0.0) {
            out << std::setprecision(0) << "Hands/Hour: " << double(hands - lastReportHands) / hours << "\n";
        }
        lastReportHands = hands;
        lastReportTime = now;
        std::cout << out.str() << std::flush;
    }

    void workerMain(unsigned t) {
        // On the heap: the range scratch makes a Worker too large for a thread stack.
        auto worker = std::make_unique<Worker>(t);
        runBlock(*worker, Phase::Warmup, 0, WARMUP_HANDS);
        for (uint64_t block = 0; block < BLOCKS; block++) {
            deltaPass(t);
            runBlock(*worker, Phase::Training, block * BLOCK, BLOCK);
        }
        deltaPass(t);
        runBlock(*worker, Phase::Cooldown, 0, COOLDOWN_HANDS);
        deltaPass(t, true);
    }

    void run() {
        lastReportTime = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned t = 0; t < threads; t++) {
            pool.emplace_back([this, t] {
                try {
                    workerMain(t);
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(printMutex);
                    std::cerr << "worker " << t << " failed: " << e.what() << std::endl;
                    std::_Exit(1);
                }
            });
        }
        for (auto& thread : pool) {
            thread.join();
        }
    }

    // The last digit names the trainer: 1 for this range trainer, 0 for single hand.
    static std::string spec(bool metasBasic, bool memoryBasic) {
        return std::to_string((int)(10000 * PLAYER_COUNT + 10 * CHIP_COUNT
            + 10 * metasBasic + 1 * memoryBasic));
    }
    
    void downloadStrategyBase() {
        std::string path = "solver/strategies/strategy_"
            + spec(BASE_METAS_BASIC, BASE_MEMORY_BASIC) + ".bin";
        uint64_t bytes = fileSize(path);
        std::ifstream file = openRead(path);
        uint64_t size;
        readExact(file, &size, sizeof(size), path);
        if (bytes != STRATEGY_HEADER_BYTES + size * STRATEGY_RECORD_BYTES) {
            throw std::runtime_error(path + " is not " + std::to_string(size) + " records of "
                + std::to_string(STRATEGY_RECORD_BYTES) + " bytes");
        }
        strategyBase.build(size);
        constexpr uint64_t BATCH = 1ULL << 16;
        std::vector<char> buffer(BATCH * STRATEGY_RECORD_BYTES);
        for (uint64_t done = 0; done < size; ) {
            uint64_t take = std::min(BATCH, size - done);
            readExact(file, buffer.data(), take * STRATEGY_RECORD_BYTES, path);
            const char* cursor = buffer.data();
            for (uint64_t i = 0; i < take; i++) {
                uint64_t key;
                std::array<float, 4> weights;
                std::memcpy(&key, cursor, sizeof(key));
                std::memcpy(weights.data(), cursor + 8, sizeof(weights));
                strategyBase.insert(key, weights);
                cursor += STRATEGY_RECORD_BYTES;
            }
            done += take;
        }
        std::cout << "Strategy base: " << strategyBase.size() << " entries in "
                  << strategyBase.slotCount() << " slots ("
                  << (strategyBase.slotCount() * sizeof(BaseEntry) / (1024.0 * 1024.0))
                  << " MB)" << std::endl;
    }

    void uploadStrategy() {
        CheckedWriter writeFile("solver/strategies/strategy_"
            + spec(METAS_BASIC, MEMORY_BASIC) + ".bin");
        uint64_t size = strategy.count();
        writeFile.write(&size, sizeof(size));
        for (uint64_t i = 0; i < size; i++) {
            Entry& weights = strategy.dense[i];
            std::array<float, 4> out;
            for (int j = 0; j < 4; j++) {
                out[j] = float(std::bit_cast<double>(
                    weights.weights[j].load(std::memory_order_relaxed)));
            }
            uint32_t visits = entryVisits(weights.stats.load(std::memory_order_relaxed));
            uint64_t key = weights.key.load(std::memory_order_relaxed);
            writeFile.write(&key, sizeof(key));
            writeFile.write(&out, sizeof(out));
            writeFile.write(&visits, sizeof(visits));
        }
        writeFile.commit();
    }

    void printSize() {
        std::cout << std::fixed << std::setprecision(3);
        uint64_t size = strategy.count();
        std::cout << "Strategy size: " << size << std::endl;
        std::cout << "Load factor: " << 100.0 * double(size) / double(StrategyTable::CAP) << "%" << std::endl;
        if (size > MAX_LOAD * double(StrategyTable::CAP)) {
            std::cout << "Overflow" << std::endl;
        }
    }
};

int main () {
    try {
        // delta encodes the node's rate per unit of visit weight, 1 / visits^FLEXIBILITY,
        // logarithmically between the rates at VISITS_CEIL (delta 0) and VISITS_FLOOR.
        MIN_DELTA_INC = 1.0 / std::pow(VISITS_CEIL, FLEXIBILITY);
        INV_MIN_DELTA_INC = 1.0 / MIN_DELTA_INC;
        DELTA_SCALAR = std::log(1.0 / std::pow(VISITS_FLOOR, FLEXIBILITY)
            * INV_MIN_DELTA_INC) / 65535.0;
        INV_DELTA_SCALAR = 1.0 / DELTA_SCALAR;

        unsigned threads = std::thread::hardware_concurrency();
        if (const char* override = std::getenv("FIREFLY_THREADS")) {
            threads = unsigned(std::strtoul(override, nullptr, 10));
        }
        if (threads == 0) {
            threads = 1;
        }
        std::cout << "Threads: " << threads << std::endl;
        std::cout << std::boolalpha << std::setprecision(12)
                  << "PLAYER_COUNT: " << PLAYER_COUNT << "\n"
                  << "CHIP_COUNT: " << CHIP_COUNT << "\n"
                  << "METAS_BASIC: " << METAS_BASIC << "\n"
                  << "MEMORY_BASIC: " << MEMORY_BASIC << "\n"
                  << "BASE_METAS_BASIC: " << BASE_METAS_BASIC << "\n"
                  << "BASE_MEMORY_BASIC: " << BASE_MEMORY_BASIC << "\n"
                  << "USE_BASE: " << USE_BASE << "\n"
                  << "BASE_CONFIDENCE: " << BASE_CONFIDENCE << "\n"
                  << "EPOCHS: " << EPOCHS << "\n"
                  << "INV_TEMP: " << INV_TEMP << "\n"
                  << "SAMPLE_INV_TEMP: " << SAMPLE_INV_TEMP << "\n"
                  << "FLEXIBILITY: " << FLEXIBILITY << "\n"
                  << "LEARNING_RATE: " << LEARNING_RATE << "\n"
                  << "VISITS_FLOOR: " << VISITS_FLOOR << "\n"
                  << "VISITS_CEIL: " << VISITS_CEIL << "\n"
                  << "RANGE_FLOOR: " << RANGE_FLOOR << std::endl;

        Engine::loadTables();

        Explorer explorer(threads);
        if (USE_BASE) {
            explorer.downloadStrategyBase();
        }
        explorer.run();
        explorer.printSize();
        explorer.uploadStrategy();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
