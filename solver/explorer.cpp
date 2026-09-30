#include "solver/config.h"
#include "solver/engine.h"
#include "solver/strategyTable.h"
#include "solver/baseTable.h"

double MIN_DELTA_INC;
double DELTA_SCALAR;
double INV_MIN_DELTA_INC;
double INV_DELTA_SCALAR;

static inline std::pair<double, double> emaUpdate(std::atomic<uint64_t>& word,
            float target, double scalar) {
    uint64_t old = word.load(std::memory_order_relaxed);
    uint64_t next;
    double value;
    do {
        value = std::bit_cast<double>(old);
        float delta = target - value;
        next = std::bit_cast<uint64_t>(value + scalar * delta);
    } while (!word.compare_exchange_weak(old, next,
        std::memory_order_relaxed, std::memory_order_relaxed));
    return {value, std::bit_cast<double>(next)};
}

static inline const char* optionName(int option) {
    switch (option) {
        case 0:  return "fold";
        case 1:  return "call/check";
        case 2:  return "raise 0.5x pot";
        case 3:  return "raise 1.5x pot";
        default: return "all in";
    }
}

// One decision of the hand. Every combo of the acting seat that was still in its
// range was looked up individually; entries holds the node each one reached, or
// nullptr for a combo that was out of the range.
struct RangeDecision {
    int seat;
    int option;
    float chips;
    float pot;
    std::array<Entry*, COMBO_COUNT> entries;
};

struct TraceNode {
    Entry* entry;
    uint64_t key;
    int decision;
    int combo;
    int action;
    int option;
    uint8_t legal;
    uint32_t visits;
    float reach;
    std::array<double, 4> weights;
    std::array<double, 5> probabilities;
    std::array<double, 5> aggregate;
    float chips;
    float pot;

    bool updated;
    double finalReach;
    double scalar;
    double target;
    double before;
    double after;
};

struct Worker {
    Engine engine;
    std::mt19937 gen;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    // Reused across hands; decisionCount is how many are live in this one.
    std::vector<RangeDecision> decisions;
    size_t decisionCount = 0;
    std::array<uint64_t, COMBO_COUNT> keys;
    std::array<std::array<double, 5>, COMBO_COUNT> policy;
    std::array<std::array<float, COMBO_COUNT>, PLAYER_COUNT> rewards;
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
    std::mutex printMutex;

    explicit Explorer(unsigned threads) : threads(threads), bar(threads), nextChunk(0) {}

    // Finds the node for one combo's key, inserting it if new. Visits are not
    // counted here: they are counted at the end of the hand, weighted by reach.
    Entry& locateNode(Engine& engine, int combo, uint64_t key, const std::array<bool, 5>& options) {
        bool inserted = false;
        uint64_t slotIndex = 0;
        uint64_t word = 0;
        Entry& weights = *strategy.locate(key, slotIndex, word, inserted);
        if (inserted) {
            const BaseEntry* base = nullptr;
            if constexpr (USE_BASE) {
                base = strategyBase.find(
                    engine.getComboState(combo, BASE_METAS_BASIC, BASE_MEMORY_BASIC));
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
                    if (options[j] && BASE_PRUNE_RATIO * draw[j] / sum >= 1.0) {
                        legal |= uint8_t(1u << j);
                    }
                }
            }
            for (int j = 0; j < 4; j++) {
                double seed = base != nullptr ? double(base->weights[j]) : 0.0;
                weights.weights[j].store(std::bit_cast<uint64_t>(seed),
                    std::memory_order_relaxed);
            }
            weights.visits.store(0, std::memory_order_relaxed);
            weights.options.store(legal, std::memory_order_relaxed);
            weights.delta.store(255, std::memory_order_relaxed);
            strategy.publish(slotIndex, word);
        }
        return weights;
    }

    // The node's policy at INV_TEMP over its legal options. Fold's weight is the
    // exact 0, so it draws exp(0) = 1.
    static void nodePolicy(const Entry& weights, std::array<double, 5>& probabilities) {
        const uint8_t legal = weights.options.load(std::memory_order_relaxed);
        double sum = 0;
        for (int j = 0; j < 5; j++) {
            double draw = 0;
            if (legal & (1u << j)) {
                double w = j == 0 ? 0.0 : std::bit_cast<double>(
                    weights.weights[j - 1].load(std::memory_order_relaxed));
                draw = std::exp(std::clamp(INV_TEMP * w, -MAX_EXP_ARG, MAX_EXP_ARG));
            }
            probabilities[j] = draw;
            sum += draw;
        }
        for (int j = 0; j < 5; j++) {
            probabilities[j] /= sum;
        }
    }

    // Samples the acting seat's action from its whole range: each combo still in the
    // range reads its own node, the policies are summed weighted by reach, and one
    // option is drawn from the sum. Every combo's reach is then multiplied by the
    // probability it had of taking that option.
    int chooseRangeOption(Worker& worker) {
        Engine& engine = worker.engine;
        const int seat = engine.getAction();
        std::array<float, COMBO_COUNT>& range = engine.players[seat].range;
        // getOptions() reads spr and cpr, which getState() writes, and neither
        // depends on the combo - so legality is shared by the whole range.
        engine.getState(METAS_BASIC, MEMORY_BASIC);
        const std::array<bool, 5> options = engine.getOptions();

        // Keys first, prefetching each home slot, so the lookups below overlap
        // their cache misses.
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (range[combo] < RANGE_FLOOR) {
                range[combo] = 0.0f;
                continue;
            }
            uint64_t key = engine.getComboState(combo, METAS_BASIC, MEMORY_BASIC);
            worker.keys[combo] = key;
            strategy.prefetch(key);
        }

        if (worker.decisionCount == worker.decisions.size()) {
            worker.decisions.emplace_back();
        }
        const int decisionIndex = int(worker.decisionCount++);
        RangeDecision& decision = worker.decisions[decisionIndex];
        decision.seat = seat;
        decision.chips = engine.getChips();
        decision.pot = engine.getPot();

        std::array<double, 5> aggregate = {};
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (range[combo] == 0.0f) {
                decision.entries[combo] = nullptr;
                continue;
            }
            Entry& weights = locateNode(engine, combo, worker.keys[combo], options);
            decision.entries[combo] = &weights;
            std::array<double, 5>& probabilities = worker.policy[combo];
            nodePolicy(weights, probabilities);
            for (int j = 0; j < 5; j++) {
                aggregate[j] += double(range[combo]) * probabilities[j];
            }
        }

        double running = 0;
        std::array<double, 5> dividers;
        for (int j = 0; j < 5; j++) {
            running += aggregate[j];
            dividers[j] = running;
        }
        if (!(running > 0)) {
            throw std::runtime_error("the acting seat's range is empty");
        }
        int option = 0;
        double sample = worker.unit(worker.gen) * running;
        for (int j = 0; j < 5; j++) {
            if (sample < dividers[j] && aggregate[j] > 0) {
                option = j;
                break;
            }
        }
        decision.option = option;

        if constexpr (TRACE_KEY_ENABLED) {
            for (int combo = 0; combo < COMBO_COUNT; combo++) {
                if (decision.entries[combo] == nullptr || worker.keys[combo] != TRACE_KEY) {
                    continue;
                }
                Entry& weights = *decision.entries[combo];
                TraceNode node{};
                node.entry = &weights;
                node.key = worker.keys[combo];
                node.decision = decisionIndex;
                node.combo = combo;
                node.action = seat;
                node.option = option;
                node.legal = weights.options.load(std::memory_order_relaxed);
                node.visits = weights.visits.load(std::memory_order_relaxed);
                node.reach = range[combo];
                for (int j = 0; j < 4; j++) {
                    node.weights[j] = std::bit_cast<double>(
                        weights.weights[j].load(std::memory_order_relaxed));
                }
                node.probabilities = worker.policy[combo];
                for (int j = 0; j < 5; j++) {
                    node.aggregate[j] = aggregate[j] / running;
                }
                node.chips = decision.chips;
                node.pot = decision.pot;
                node.updated = false;
                worker.tracePath.push_back(node);
            }
        }

        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (decision.entries[combo] != nullptr) {
                range[combo] *= float(worker.policy[combo][option]);
            }
        }
        return option;
    }

    void printTrace(Worker& worker) {
        if (worker.tracePath.empty()) {
            return;
        }
        std::ostringstream out;
        out << std::fixed;
        for (const TraceNode& node : worker.tracePath) {
            const std::array<int, 2>& cards = comboTables.cards[node.combo];
            out << "===== trace key 0x" << std::hex << std::setw(16) << std::setfill('0')
                << node.key << std::dec << std::setfill(' ') << " =====\n";
            out << std::setprecision(3);
            out << "  seat " << node.action << "   combo " << node.combo
                << " (cards " << cards[0] << ", " << cards[1] << ")"
                << "   visits " << node.visits << "   invTemp " << INV_TEMP
                << "   chips " << node.chips << "   pot " << node.pot << "\n";
            out << std::setprecision(6);
            out << "  reach     at decision " << node.reach
                << "   at hand end " << node.finalReach << "\n";
            out << "  weights ";
            for (int j = 0; j < 5; j++) {
                out << "  " << optionName(j) << " ";
                if (!(node.legal & (1u << j))) {
                    out << "-";
                } else if (j == 0) {
                    out << "0.000000 (exact)";
                } else {
                    out << node.weights[j - 1];
                }
            }
            out << "\n";
            out << std::setprecision(4);
            out << "  policy  ";
            for (int j = 0; j < 5; j++) {
                out << "  " << optionName(j) << " " << 100.0 * node.probabilities[j] << "%";
            }
            out << "\n";
            out << "  range   ";
            for (int j = 0; j < 5; j++) {
                out << "  " << optionName(j) << " " << 100.0 * node.aggregate[j] << "%";
            }
            out << "\n";
            out << "  chosen    " << optionName(node.option) << "\n";
            out << std::setprecision(6);
            if (node.option == 0) {
                out << "  update    none, fold has no stored weight\n";
            } else if (!node.updated) {
                out << "  update    none, the phase multiplier or the final reach is 0, "
                       "or the combo clashes with the board\n";
            } else {
                out << "  return    " << node.target << " of the pot\n";
                out << "  update    " << optionName(node.option) << " "
                    << node.before << " -> " << node.after
                    << "  (change " << (node.after - node.before)
                    << ", step " << node.scalar << ")\n";
            }
        }
        worker.tracePath.clear();
        std::lock_guard<std::mutex> lock(printMutex);
        std::cout << out.str() << std::flush;
    }

    // At the end of the hand every combo that reached a decision takes one EMA step
    // toward its own pot normalised return, scaled by its seat's final reach for it
    // - the likelihood that combo played the whole line - so lines a combo would
    // not have taken barely move it. The same reach decides, as a probability,
    // whether the touch counts as a visit, which keeps the step size calibrated to
    // the reach mass a node actually receives. Combos that clash with any of the five
    // board cards are skipped, whether or not the hand reached them.
    void updateRangeWeights(Worker& worker, double multiplier) {
        Engine& engine = worker.engine;
        engine.getComboRewards(worker.rewards);
        std::array<bool, COMBO_COUNT> onBoard;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            onBoard[combo] = engine.comboConflicts(combo, 5);
        }
        for (size_t d = 0; d < worker.decisionCount; d++) {
            const RangeDecision& decision = worker.decisions[d];
            const std::array<float, COMBO_COUNT>& finalRange = engine.players[decision.seat].range;
            const std::array<float, COMBO_COUNT>& rewards = worker.rewards[decision.seat];
            for (int combo = 0; combo < COMBO_COUNT; combo++) {
                Entry* weights = decision.entries[combo];
                if (weights == nullptr || onBoard[combo]) {
                    continue;
                }
                double reach = finalRange[combo];
                if (!(reach > 0)) {
                    continue;
                }
                if (worker.unit(worker.gen) < reach) {
                    if (weights->visits.fetch_add(1, std::memory_order_relaxed) == VISITS_MAX) {
                        throw std::runtime_error("a node reached 2^32 visits, so the visit "
                            "counter wrapped and its step size would jump to the maximum");
                    }
                }
                if (decision.option == 0 || multiplier == 0.0) {
                    continue;
                }
                double scalar = multiplier * reach * MIN_DELTA_INC * std::exp(DELTA_SCALAR
                    * weights->delta.load(std::memory_order_relaxed));
                float target = (rewards[combo] - decision.chips) / decision.pot;
                auto [before, after] = emaUpdate(
                    weights->weights[decision.option - 1], target, scalar);
                if constexpr (TRACE_KEY_ENABLED) {
                    for (TraceNode& node : worker.tracePath) {
                        if (node.decision == int(d) && node.combo == combo) {
                            node.updated = true;
                            node.scalar = scalar;
                            node.target = target;
                            node.before = before;
                            node.after = after;
                        }
                    }
                }
            }
        }
        if constexpr (TRACE_KEY_ENABLED) {
            for (TraceNode& node : worker.tracePath) {
                node.finalReach = engine.players[node.action].range[node.combo];
            }
            printTrace(worker);
        }
        worker.decisionCount = 0;
    }

    void updateDeltasRange(uint64_t begin, uint64_t end) {
        for (uint64_t i = begin; i < end; i++) {
            Entry& weights = strategy.dense[i];
            uint32_t visits = weights.visits.load(std::memory_order_relaxed);
            double alpha = -std::expm1(-1.0 / std::max(double(visits), double(VISIT_FLOOR)));
            long encoded = std::lround(std::log(alpha * INV_MIN_DELTA_INC) * INV_DELTA_SCALAR);
            weights.delta.store(uint8_t(std::clamp(encoded, 0L, 255L)),
                std::memory_order_relaxed);
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
        }
        bar.arrive_and_wait();
    }

    void playHand(Worker& worker, double multiplier) {
        Engine& engine = worker.engine;
        engine.shuffle(worker.gen);
        engine.dealRanges();
        worker.decisionCount = 0;
        int option;
        do {
            option = chooseRangeOption(worker);
        } while (engine.play(option));
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
            }
            if (chunk % PRINT_CHUNKS == 0) {
                report(phase, blockBase + base);
            }
        }
    }

    void report(Phase phase, uint64_t globalHand) {
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
        std::lock_guard<std::mutex> lock(printMutex);
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

    static std::string spec(bool metasBasic, bool memoryBasic) {
        return std::to_string((int)(10000 * PLAYER_COUNT + 10 * CHIP_COUNT
            + 10 * metasBasic + memoryBasic));
    }
    
    void downloadStrategyBase() {
        std::string path = "solver/strategies/strategy_"
            + spec(BASE_METAS_BASIC, BASE_MEMORY_BASIC) + ".bin";
        uint64_t bytes = fileSize(path);
        std::ifstream file = openRead(path);
        uint64_t size;
        readExact(file, &size, sizeof(size), path);
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
            uint32_t visits = weights.visits.load(std::memory_order_relaxed);
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
        MIN_DELTA_INC = -std::expm1(-1.0 / 4294967295.0);
        INV_MIN_DELTA_INC = 1.0 / MIN_DELTA_INC;
        const double maxAlpha = -std::expm1(-1.0 / 5.0);
        DELTA_SCALAR = std::log(maxAlpha * INV_MIN_DELTA_INC) / 255.0;
        INV_DELTA_SCALAR = 1.0 / DELTA_SCALAR;

        unsigned threads = std::thread::hardware_concurrency();
        if (const char* override = std::getenv("FIREFLY_THREADS")) {
            threads = unsigned(std::strtoul(override, nullptr, 10));
        }
        if (threads == 0) {
            threads = 1;
        }
        std::cout << "Threads: " << threads << std::endl;

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
