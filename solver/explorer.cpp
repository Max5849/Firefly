#include "solver/config.h"
#include "solver/engine.h"
#include "solver/strategyTable.h"
#include "solver/baseTable.h"

inline constexpr double MAX_EXP_ARG = 700.0;

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

struct EpochNode {
    Entry* entry;
    int option;
    int action;
    float chips;
    float pot;
    int trace;
};

struct TraceNode {
    Entry* entry;
    uint64_t key;
    int action;
    int option;
    uint8_t legal;
    uint32_t visits;
    double invTemp;
    std::array<double, 4> weights;
    std::array<double, 5> probabilities;
    float chips;
    float pot;

    bool updated;
    double scalar;
    double target;
    double before;
    double after;
};

struct Worker {
    Engine engine;
    std::mt19937 gen;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    std::vector<EpochNode> epochPath;
    std::vector<TraceNode> tracePath;

    explicit Worker(unsigned t) : gen(15 + t) {
        epochPath.reserve(64);
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

    int chooseOption(Worker& worker, uint64_t keyBase, uint64_t key, float pot,
                std::array<bool, 5> options, int action, float chips, double invTemp) {
        bool inserted = false;
        uint64_t slotIndex = 0;
        uint64_t word = 0;
        Entry& weights = *strategy.locate(key, slotIndex, word, inserted);
        if (inserted) {
            const BaseEntry* base = USE_BASE ? strategyBase.find(keyBase) : nullptr;
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
                            std::log(MAX_INV_TEMP) * double(base->weights[j - 1]),
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
        if (weights.visits.fetch_add(1, std::memory_order_relaxed) == VISITS_MAX) {
            throw std::runtime_error("a node reached 2^32 visits, so the visit counter "
                "wrapped and its step size would jump to the maximum");
        }
        const uint8_t legal = weights.options.load(std::memory_order_relaxed);
        std::array<double, 5> dividers = {0};
        double running = 0;
        if (legal & 1u) {
            running += 1.0;
        }
        dividers[0] = running;
        for (int j = 1; j < 5; j++) {
            if (legal & (1u << j)) {
                double w = std::bit_cast<double>(
                    weights.weights[j - 1].load(std::memory_order_relaxed));
                running += std::exp(std::clamp(invTemp * w, -MAX_EXP_ARG, MAX_EXP_ARG));
            }
            dividers[j] = running;
        }
        int option = 0;
        double sample = worker.unit(worker.gen) * running;
        for (int j = 0; j < 5; j++) {
            if (sample < dividers[j]) {
                option = j;
                break;
            }
        }
        int trace = -1;
        if constexpr (TRACE_KEY_ENABLED) {
            if (key == TRACE_KEY) {
                TraceNode node{};
                node.entry = &weights;
                node.key = key;
                node.action = action;
                node.option = option;
                node.legal = legal;
                node.visits = weights.visits.load(std::memory_order_relaxed);
                node.invTemp = invTemp;
                for (int j = 0; j < 4; j++) {
                    node.weights[j] = std::bit_cast<double>(
                        weights.weights[j].load(std::memory_order_relaxed));
                }
                for (int j = 0; j < 5; j++) {
                    double mass = dividers[j] - (j == 0 ? 0.0 : dividers[j - 1]);
                    node.probabilities[j] = running > 0 ? mass / running : 0.0;
                }
                node.chips = chips;
                node.pot = pot;
                node.updated = false;
                trace = int(worker.tracePath.size());
                worker.tracePath.push_back(node);
            }
        }
        if (option != 0) {
            worker.epochPath.push_back({&weights, option, action, chips, pot, trace});
        }
        return option;
    }

    void printTrace(Worker& worker, std::array<float, PLAYER_COUNT> rewards) {
        if (worker.tracePath.empty()) {
            return;
        }
        std::ostringstream out;
        out << std::fixed;
        for (const TraceNode& node : worker.tracePath) {
            out << "===== trace key 0x" << std::hex << std::setw(16) << std::setfill('0')
                << node.key << std::dec << std::setfill(' ') << " =====\n";
            out << std::setprecision(3);
            out << "  seat " << node.action << "   visits " << node.visits
                << "   invTemp " << node.invTemp
                << "   chips " << node.chips << "   pot " << node.pot << "\n";
            out << std::setprecision(6);
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
            out << "  chosen    " << optionName(node.option) << "\n";
            out << std::setprecision(6);
            if (node.option == 0) {
                out << "  return    0.000000 (folding ends the seat's hand at the stack "
                       "it had here)\n";
                out << "  update    none, fold has no stored weight\n";
            } else {
                out << "  return    stack " << (rewards[node.action] - node.chips)
                    << " over pot " << node.pot << " = " << node.target << "\n";
                if (!node.updated) {
                    out << "  update    none, the phase multiplier is 0\n";
                } else {
                    out << "  update    " << optionName(node.option) << " "
                        << node.before << " -> " << node.after
                        << "  (change " << (node.after - node.before)
                        << ", step " << node.scalar << ")\n";
                }
            }
        }
        worker.tracePath.clear();
        std::lock_guard<std::mutex> lock(printMutex);
        std::cout << out.str() << std::flush;
    }

    void updateWeights(Worker& worker, std::array<float, PLAYER_COUNT> rewards, double multiplier) {
        if (multiplier != 0.0) {
            for (auto& epochNode : worker.epochPath) {
                Entry* weights = epochNode.entry;
                double scalar = multiplier * MIN_DELTA_INC * std::exp(DELTA_SCALAR
                    * weights->delta.load(std::memory_order_relaxed));
                float target = (rewards[epochNode.action] - epochNode.chips) / epochNode.pot;
                auto [before, after] = emaUpdate(
                    weights->weights[epochNode.option - 1], target, scalar);
                if constexpr (TRACE_KEY_ENABLED) {
                    if (epochNode.trace >= 0) {
                        TraceNode& node = worker.tracePath[epochNode.trace];
                        node.updated = true;
                        node.scalar = scalar;
                        node.target = target;
                        node.before = before;
                        node.after = after;
                    }
                }
            }
        }
        worker.epochPath.clear();
        if constexpr (TRACE_KEY_ENABLED) {
            for (TraceNode& node : worker.tracePath) {
                if (!node.updated && node.option != 0) {
                    node.target = (rewards[node.action] - node.chips) / node.pot;
                }
            }
            printTrace(worker, rewards);
        }
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

    void playHand(Worker& worker, double invTemp, double multiplier) {
        Engine& engine = worker.engine;
        engine.shuffle(worker.gen);
        engine.deal();
        int option;
        do {
            uint64_t keyBase = USE_BASE ? engine.getState(BASE_METAS_BASIC, BASE_MEMORY_BASIC) : 0;
            uint64_t key = engine.getState(METAS_BASIC, MEMORY_BASIC);
            option = chooseOption(worker, keyBase, key, engine.getPot(), engine.getOptions(),
                engine.getAction(), engine.getChips(), invTemp);
        } while (engine.play(option));
        engine.showdown();
        updateWeights(worker, engine.getRewards(), multiplier);
    }

    void runBlock(Worker& worker, Phase phase, uint64_t blockBase, double invTemp,
                uint64_t phaseHands) {
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
                playHand(worker, invTemp, multiplier);
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
        Worker worker(t);
        runBlock(worker, Phase::Warmup, 0, 0.0, WARMUP_HANDS);
        for (uint64_t block = 0; block < BLOCKS; block++) {
            deltaPass(t);
            runBlock(worker, Phase::Training, block * BLOCK,
                std::log(MAX_INV_TEMP) * double(block) / double(BLOCKS), BLOCK);
        }
        deltaPass(t);
        runBlock(worker, Phase::Cooldown, 0, std::log(MAX_INV_TEMP), COOLDOWN_HANDS);
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
