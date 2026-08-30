#include "solver/config.h"
#include "solver/engine.h"
#include "solver/strategyTable.h"
#include "solver/baseTable.h"

// Step size encoding, pinned to visit counts rather than tuned: delta 0 is the
// step at the largest representable visit count, delta 255 the step at the
// max(visits, 5) floor.
double MIN_DELTA_INC;
double DELTA_SCALAR;
double INV_MIN_DELTA_INC;
double INV_DELTA_SCALAR;

// w += s * (target - w). Recomputing the step from the freshly loaded value on
// every retry makes two concurrent updates compose into exactly two sequential
// EMA steps, so nothing is lost and no lock is needed.
static inline void emaUpdate(std::atomic<uint64_t>& word, float target, double scalar) {
    uint64_t old = word.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        double value = std::bit_cast<double>(old);
        float delta = target - value;
        next = std::bit_cast<uint64_t>(value + scalar * delta);
    } while (!word.compare_exchange_weak(old, next,
        std::memory_order_relaxed, std::memory_order_relaxed));
}

struct EpochNode {
    Entry* entry;
    int option;
    int action;
    float chips;
};

struct Worker {
    Engine engine;
    std::mt19937 gen;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    std::vector<EpochNode> epochPath;

    // Thread 0 reproduces the original single-threaded stream, so a one thread run
    // stays comparable against earlier results.
    explicit Worker(unsigned t) : gen(15 + t) {
        epochPath.reserve(64);
    }
};

enum class Phase { Warmup, Training, Cooldown };

struct Explorer {
    // Warm up and cooldown are 2% of EPOCHS each and each runs as one
    // uninterrupted stretch, so neither takes a delta update at its midpoint.
    static constexpr uint64_t BLOCK = EPOCHS / 100;
    static constexpr uint64_t WARMUP_HANDS = EPOCHS / 50;
    static constexpr uint64_t COOLDOWN_HANDS = EPOCHS / 50;

    StrategyTable strategy;
    BaseTable strategyBase;
    unsigned threads;
    std::barrier<> bar;
    std::atomic<uint64_t> nextChunk;
    std::mutex printMutex;

    explicit Explorer(unsigned threads) : threads(threads), bar(threads), nextChunk(0) {}

    int chooseOption(Worker& worker, uint64_t keyBase, uint64_t key,
                std::array<bool, 5> options, int action, float chips, double invTemp) {
        bool inserted = false;
        uint64_t slotIndex = 0;
        uint64_t word = 0;
        Entry& weights = *strategy.locate(key, slotIndex, word, inserted);
        if (inserted) {
            // The mapping is already zero, so only the legal options and the
            // starting step need writing before the record is published.
            const int baseOptions = strategyBase.find(keyBase);
            const bool baseLost = baseOptions < 0;
            uint8_t legal = 0;
            for (int j = 0; j < 5; j++) {
                if (options[j] && (baseLost || (baseOptions & (1 << j)))) {
                    legal |= uint8_t(1u << j);
                }
            }
            weights.options.store(legal, std::memory_order_relaxed);
            weights.delta.store(255, std::memory_order_relaxed);
            strategy.publish(slotIndex, word);
        }
        if (weights.visits.fetch_add(1, std::memory_order_relaxed) == VISITS_MAX) {
            throw std::runtime_error("a node reached 2^32 visits, so the visit counter "
                "wrapped and its step size would jump to the maximum");
        }
        // The sampler is stochastic and legality is fixed at insert, so this read
        // path needs nothing beyond relaxed atomics. Running totals let one uniform
        // sample scaled by the total land in exactly one option's interval.
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
                running += std::exp(invTemp * std::copysign(8.33, w)
                    * std::log(1 + std::abs(w) / 8.33));
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
        if (option != 0) {
            worker.epochPath.push_back({&weights, option, action, chips});
        }
        return option;
    }

    void updateWeights(Worker& worker, std::array<float, PLAYER_COUNT> rewards, double multiplier) {
        // Warm up passes zero, making every step a no-op, so that phase produces no
        // weight write traffic at all.
        if (multiplier != 0.0) {
            for (auto& epochNode : worker.epochPath) {
                Entry* weights = epochNode.entry;
                double scalar = multiplier * MIN_DELTA_INC * std::exp(DELTA_SCALAR
                    * weights->delta.load(std::memory_order_relaxed));
                emaUpdate(weights->weights[epochNode.option - 1],
                    rewards[epochNode.action] - epochNode.chips, scalar);
            }
        }
        worker.epochPath.clear();
    }

    // Runs at a barrier over a record range owned exclusively by this thread.
    // Walking the dense array costs time proportional to the states that exist
    // rather than to the capacity.
    void updateDeltasRange(uint64_t begin, uint64_t end) {
        for (uint64_t i = begin; i < end; i++) {
            Entry& weights = strategy.dense[i];
            uint32_t visits = weights.visits.load(std::memory_order_relaxed);
            // max(visits, 5) pins the top of the byte at 255; the counter's own
            // ceiling pins the bottom at 0. The clamp only guards the last bit of
            // floating point rounding.
            double alpha = -std::expm1(-std::log(2) / std::max(double(visits), 5.0));
            long encoded = std::lround(std::log(alpha * INV_MIN_DELTA_INC) * INV_DELTA_SCALAR);
            weights.delta.store(uint8_t(std::clamp(encoded, 0L, 255L)),
                std::memory_order_relaxed);
            weights.visits.store(0, std::memory_order_relaxed);
        }
    }

    // Quiesce every thread, rebuild the deltas over disjoint ranges, rearm the
    // chunk counter, then release everyone.
    void deltaPass(unsigned t) {
        bar.arrive_and_wait();
        uint64_t live = strategy.count();
        uint64_t span = live / threads;
        uint64_t begin = span * t;
        uint64_t end = (t + 1 == threads) ? live : span * (t + 1);
        updateDeltasRange(begin, end);
        if (t == 0) {
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
            option = chooseOption(worker, keyBase, key, engine.getOptions(),
                engine.getAction(), engine.getChips(), invTemp);
        } while (engine.play(option));
        engine.showdown();
        updateWeights(worker, engine.getRewards(), multiplier);
    }

    // One continuous stretch of phaseHands hands, claimed CHUNK_HANDS at a time so
    // the performance cores simply take more chunks than the efficiency cores.
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
        // The inverse temperature is held for a whole block and steps at the block
        // boundary, in step with the delta pass that opens it, rising from 0 to its
        // ceiling of log(4) across the hundred blocks.
        for (uint64_t block = 0; block < 100; block++) {
            deltaPass(t);
            runBlock(worker, Phase::Training, block * BLOCK,
                std::log(1.0 + 3.0 * double(block) / 100.0), BLOCK);
        }
        deltaPass(t);
        runBlock(worker, Phase::Cooldown, 0, std::log(4.0), COOLDOWN_HANDS);
    }

    void run() {
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned t = 0; t < threads; t++) {
            pool.emplace_back([this, t] {
                try {
                    workerMain(t);
                } catch (const std::exception& e) {
                    // A table overflow trips every worker at once. Taking the print
                    // lock and exiting while still holding it means exactly one
                    // legible message instead of ten interleaved ones. Unwinding
                    // would leave the others deadlocked at the barrier.
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
        std::string path = "strategies/strategyBase_"
            + spec(BASE_METAS_BASIC, BASE_MEMORY_BASIC) + ".bin";
        uint64_t bytes = fileSize(path);
        std::ifstream file = openRead(path);
        uint64_t size;
        readExact(file, &size, sizeof(size), path);
        // Validating the count against the file length also stops a corrupt header
        // from asking for an absurd allocation.
        if (bytes != sizeof(uint64_t) + size * 9) {
            throw std::runtime_error(path + " has an unexpected size");
        }
        strategyBase.build(size);
        // Streamed rather than read whole: at full scale the file is 2.3 GB, and
        // holding it beside the table it fills would double peak memory.
        constexpr uint64_t BATCH = 1ULL << 16;
        std::vector<char> buffer(BATCH * 9);
        for (uint64_t done = 0; done < size; ) {
            uint64_t take = std::min(BATCH, size - done);
            readExact(file, buffer.data(), take * 9, path);
            const char* cursor = buffer.data();
            for (uint64_t i = 0; i < take; i++) {
                uint64_t key;
                std::memcpy(&key, cursor, sizeof(key));
                strategyBase.insert(key, uint8_t(cursor[8]));
                cursor += 9;
            }
            done += take;
        }
        std::cout << "Strategy base: " << strategyBase.size() << " entries in "
                  << strategyBase.slots() << " slots ("
                  << (strategyBase.slots() * 9 / (1024.0 * 1024.0)) << " MB)" << std::endl;
    }

    void uploadStrategy() {
        CheckedWriter writeFile("strategies/strategy_"
            + spec(METAS_BASIC, MEMORY_BASIC) + ".bin");
        uint64_t size = strategy.count();
        writeFile.write(&size, sizeof(size));
        for (uint64_t i = 0; i < size; i++) {
            Entry& weights = strategy.dense[i];
            std::array<double, 4> out;
            for (int j = 0; j < 4; j++) {
                out[j] = std::bit_cast<double>(weights.weights[j].load(std::memory_order_relaxed));
            }
            uint32_t visits = weights.visits.load(std::memory_order_relaxed);
            writeFile.write(&weights.key, sizeof(weights.key));
            writeFile.write(&out, sizeof(out));
            writeFile.write(&visits, sizeof(visits));
        }
        writeFile.commit();
    }

    void uploadStrategyBase() {
        CheckedWriter writeFile("strategies/strategyBase_"
            + spec(METAS_BASIC, MEMORY_BASIC) + ".bin");
        uint64_t size = strategy.count();
        writeFile.write(&size, sizeof(size));
        for (uint64_t i = 0; i < size; i++) {
            Entry& weights = strategy.dense[i];
            // Evaluated at the terminal inverse temperature, keeping any option
            // drawn at least once in 65536.
            const uint8_t legal = weights.options.load(std::memory_order_relaxed);
            std::array<double, 5> drawWeights = {0};
            double sum = 0;
            if (legal & 1u) {
                drawWeights[0] = 1;
            }
            sum += drawWeights[0];
            for (int j = 1; j < 5; j++) {
                if (legal & (1u << j)) {
                    double w = std::bit_cast<double>(
                        weights.weights[j - 1].load(std::memory_order_relaxed));
                    drawWeights[j] = std::exp(std::log(4) * std::copysign(8.33, w)
                        * std::log(1 + std::abs(w) / 8.33));
                }
                sum += drawWeights[j];
            }
            uint8_t options = 0;
            for (int j = 0; j < 5; j++) {
                if (65536 * drawWeights[j] / sum >= 1.0) {
                    options |= uint8_t(1u << j);
                }
            }
            writeFile.write(&weights.key, sizeof(weights.key));
            writeFile.write(&options, sizeof(options));
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
        MIN_DELTA_INC = -std::expm1(-std::log(2) / 4294967295.0);
        INV_MIN_DELTA_INC = 1.0 / MIN_DELTA_INC;
        const double maxAlpha = -std::expm1(-std::log(2) / 5.0);
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

        // Loaded before any worker exists so the I/O is not serialised behind a
        // std::call_once wait.
        Engine::loadTables();

        Explorer explorer(threads);
        if (USE_BASE) {
            explorer.downloadStrategyBase();
        }
        explorer.run();
        explorer.printSize();
        explorer.uploadStrategy();
        explorer.uploadStrategyBase();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
