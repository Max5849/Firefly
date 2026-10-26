#include "solver/singleHand/config.h"
#include "solver/singleHand/engine.h"
#include "solver/singleHand/strategyTable.h"
#include "solver/singleHand/baseTable.h"
#include "solver/singleHand/trace.h"

// Applies w <- scale * w + shift atomically. An EMA step w <- w + s (t - w) is the map
// with scale 1 - s and shift s * t.
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

// Applies v <- scale[o] * v + shift[o] to each legal option's variance in one CAS,
// rounding each back to a code with its uniform u[o]. Returns the word written.
static inline uint64_t affineUpdateVariances(std::atomic<uint64_t>& word, uint8_t legal,
        const std::array<double, 4>& scale, const std::array<double, 4>& shift,
        const std::array<double, 4>& u) {
    uint64_t old = word.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = old;
        for (int o = 0; o < 4; o++) {
            if (!(legal & (2u << o))) {
                continue;
            }
            const uint64_t code = encodeVariance(scale[o] * decodeVariance(old, o) + shift[o], u[o]);
            next = (next & ~(0xFFFFULL << (16 * o))) | code << (16 * o);
        }
    } while (!word.compare_exchange_weak(old, next,
        std::memory_order_relaxed, std::memory_order_relaxed));
    return next;
}

struct Worker {
    Engine engine;
    std::mt19937 gen;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    std::vector<TraceNode> tracePath;

    explicit Worker(unsigned t) : gen(15 + t) {}
};

struct Explorer {
    StrategyTable strategy;
    BaseTable strategyBase;
    unsigned threads;
    std::atomic<uint64_t> nextChunk;
    // Chunks that have finished, as opposed to been claimed.
    std::atomic<uint64_t> doneChunks;
    // Hands finished over the whole run. On its own cache line, since
    // every thread bumps it once a hand (128 bytes covers Apple silicon's line size).
    alignas(128) std::atomic<uint64_t> totalHands;
    // Hands that reached TRACE_KEY, so printTrace can keep one in TRACE_EVERY.
    alignas(128) std::atomic<uint64_t> traceCount;
    alignas(128) std::mutex printMutex;
    // The hand count and time at the last report, guarded by printMutex.
    uint64_t lastReportHands;
    std::chrono::steady_clock::time_point lastReportTime;

    explicit Explorer(unsigned threads) : threads(threads), nextChunk(0), doneChunks(0),
        totalHands(0), traceCount(0), lastReportHands(0),
        lastReportTime(std::chrono::steady_clock::now()) {}

    // Finds the node for one key, inserting it if new. Visits are not counted here: they
    // are counted at the traverser's decisions, weighted by its reach.
    Entry& locateNode(Engine& engine, uint64_t key, const std::array<bool, 5>& options) {
        bool inserted = false;
        uint64_t slotIndex = 0;
        uint64_t word = 0;
        Entry& weights = *strategy.locate(key, slotIndex, word, inserted);
        if (inserted) {
            const BaseEntry* base = nullptr;
            if constexpr (USE_BASE) {
                base = strategyBase.find(engine.getActionState(BASE_MEMORY_BASIC)
                    | engine.getMetaState(BASE_METAS_BASIC) << 42);
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
            // A seeded node starts from the base's weights and variances, and its options
            // skip the running mean.
            for (int j = 0; j < 4; j++) {
                storeDouble(weights.weights[j], base != nullptr ? double(base->weights[j]) : 0.0);
            }
            weights.variances.store(base != nullptr ? base->variances : 0, std::memory_order_relaxed);
            weights.stats.store(legal | (base != nullptr ? SEEDED_BIT : 0), std::memory_order_relaxed);
            strategy.publish(slotIndex, word);
        }
        return weights;
    }

    // The node's policy at INV_TEMP over its legal options. Fold's weight is the exact 0,
    // so it draws exp(0) = 1.
    static void nodePolicy(const Entry& weights, std::array<double, 5>& probabilities) {
        const uint8_t legal = entryLegal(weights);
        double sum = 0;
        for (int j = 0; j < 5; j++) {
            double draw = 0;
            if (legal & (1u << j)) {
                const double w = j == 0 ? 0.0 : loadDouble(weights.weights[j - 1]);
                draw = std::exp(std::clamp(INV_TEMP * w, -MAX_EXP_ARG, MAX_EXP_ARG));
            }
            probabilities[j] = draw;
            sum += draw;
        }
        for (int j = 0; j < 5; j++) {
            probabilities[j] /= sum;
        }
    }

    int drawOption(Worker& worker, const std::array<double, 5>& policy) {
        double sample = worker.unit(worker.gen);
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

    static void setActionStates(Engine& engine) {
        if (!MEMORY_BASIC || (USE_BASE && !BASE_MEMORY_BASIC)) {
            engine.setActionState(false);
        }
        if (MEMORY_BASIC || (USE_BASE && BASE_MEMORY_BASIC)) {
            engine.setActionState(true);
        }
    }

    // Plays option and runs the hand on to its next decision. False when the hand is over
    // and its pot has been paid out.
    static bool advance(Engine& engine, int option) {
        if (engine.play(option)) {
            return true;
        }
        if (!engine.beginNewRound()) {
            engine.showdown();
            return false;
        }
        engine.setMetaState(!METAS_BASIC || (USE_BASE && !BASE_METAS_BASIC),
            METAS_BASIC || (USE_BASE && BASE_METAS_BASIC));
        return true;
    }

    void printTrace(Worker& worker) {
        if (worker.tracePath.empty()) {
            return;
        }
        // Skipped traces are dropped before they are formatted.
        if (traceCount.fetch_add(1, std::memory_order_relaxed) % TRACE_EVERY != 0) {
            worker.tracePath.clear();
            return;
        }
        std::ostringstream out;
        formatTrace(worker.tracePath, out);
        worker.tracePath.clear();
        std::lock_guard<std::mutex> lock(printMutex);
        std::cout << out.str() << std::flush;
    }

    double traverse(Worker& worker, Engine& engine, int traverser, double reach) {
        while (true) {
            setActionStates(engine);
            // setActionState() wrote spr and cpr, which getOptions() reads.
            const std::array<bool, 5> options = engine.getOptions();
            const uint64_t key = engine.getActionState(MEMORY_BASIC)
                | engine.getMetaState(METAS_BASIC) << 42;
            Entry& weights = locateNode(engine, key, options);
            std::array<double, 5> policy;
            nodePolicy(weights, policy);
            if (engine.getAction() == traverser) {
                return explore(worker, engine, traverser, reach, weights, key, policy);
            }
            if (!advance(engine, drawOption(worker, policy))) {
                return engine.players[traverser].chips;
            }
        }
    }

    double explore(Worker& worker, Engine& engine, int traverser, double reach,
            Entry& weights, uint64_t key, const std::array<double, 5>& policy) {
        const uint8_t legal = entryLegal(weights);
        const float chips = engine.getChips();
        const float pot = engine.getPot();
        std::array<double, 5> ends;
        ends.fill(chips);
        double value = 0;
        for (int j = 0; j < 5; j++) {
            if (!(legal & (1u << j))) {
                continue;
            }
            if (j > 0) {
                Engine branch = engine;
                ends[j] = advance(branch, j)
                    ? traverse(worker, branch, traverser, reach * policy[j])
                    : branch.players[traverser].chips;
            }
            value += policy[j] * ends[j];
        }

        // The visit's weight is the traverser's reach, stochastically rounded to
        // 1 / VISITS_ONE, and it is the only weight the update uses: the visits, the
        // running mean's share and the EMA's step all read it.
        const double mass = VISITS_ONE * reach;
        uint64_t add = uint64_t(mass);
        add += worker.unit(worker.gen) < mass - double(add);
        if (add == 0) {
            return value;
        }
        const double touch = double(add) / VISITS_ONE;
        const double share = double(add) / double(addVisits(weights, add));
        const bool seeded = entrySeeded(weights);
        const uint64_t packed = weights.variances.load(std::memory_order_relaxed);

        TraceNode* node = nullptr;
        if constexpr (TRACE_KEY_ENABLED) {
            if (key == TRACE_KEY) {
                node = &worker.tracePath.emplace_back();
                node->key = key;
                node->combo = engine.getHoleCombo(traverser);
                node->seat = traverser;
                node->legal = legal;
                node->seeded = seeded;
                node->visits = double(entryVisitUnits(weights)) / VISITS_ONE;
                node->share = share;
                for (int o = 0; o < 4; o++) {
                    node->variances[o] = decodeVariance(packed, o);
                }
                node->reach = reach;
                node->probabilities = policy;
                node->chips = chips;
                node->pot = pot;
                node->value = (value - chips) / pot;
            }
        }

        // Each option's step is the running mean's share until that is smaller than the
        // EMA's, which is when the node's visits reach variance / LEARNING_RATE. A seeded
        // option starts on the EMA. The variance is centred on the weight and takes the
        // same step: v <- (1 - a) (v + a d^2), with d the return's gap to the weight
        // before the step, which is weighted Welford while a is the share.
        std::array<double, 4> scale;
        std::array<double, 4> shift;
        std::array<double, 4> u;
        scale.fill(1.0);
        shift.fill(0.0);
        for (int j = 1; j < 5; j++) {
            if (!(legal & (1u << j))) {
                continue;
            }
            const int o = j - 1;
            // The return from the decision on, as a share of the pot after calling.
            const double target = (ends[j] - chips) / pot;
            const double variance = std::max(decodeVariance(packed, o), VARIANCE_MIN);
            // A touch of weight w is w unit steps: it keeps exp(-rate * w).
            const double ema = -std::expm1(-LEARNING_RATE * touch / variance);
            const double step = seeded ? ema : std::max(share, ema);
            auto [before, after] = affineUpdate(weights.weights[o], 1.0 - step, step * target);
            const double gap = target - before;
            scale[o] = 1.0 - step;
            shift[o] = (1.0 - step) * step * gap * gap;
            u[o] = worker.unit(worker.gen);
            if constexpr (TRACE_KEY_ENABLED) {
                if (node != nullptr) {
                    node->running[o] = step == share && !seeded;
                    node->step[o] = step;
                    node->target[o] = target;
                    node->before[o] = before;
                    node->after[o] = after;
                }
            }
        }
        const uint64_t written = affineUpdateVariances(weights.variances, legal, scale, shift, u);
        if constexpr (TRACE_KEY_ENABLED) {
            if (node != nullptr) {
                node->updated = true;
                for (int o = 0; o < 4; o++) {
                    node->variancesAfter[o] = decodeVariance(written, o);
                }
            }
        }
        return value;
    }

    void playHand(Worker& worker) {
        Engine& engine = worker.engine;
        engine.shuffle(worker.gen);
        engine.deal();
        for (int traverser = 0; traverser < PLAYER_COUNT; traverser++) {
            Engine hand = engine;
            traverse(worker, hand, traverser, 1.0);
        }
        if constexpr (TRACE_KEY_ENABLED) {
            printTrace(worker);
        }
    }

    void runTraining(Worker& worker) {
        constexpr uint64_t chunks = (EPOCHS + CHUNK_HANDS - 1) / CHUNK_HANDS;
        for (;;) {
            uint64_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= chunks) {
                return;
            }
            const uint64_t base = chunk * CHUNK_HANDS;
            uint64_t hands = std::min(CHUNK_HANDS, EPOCHS - base);
            for (uint64_t j = 0; j < hands; j++) {
                playHand(worker);
                // Counted per hand, not per chunk, so a report's rate is not quantised to chunks.
                totalHands.fetch_add(1, std::memory_order_relaxed);
            }
            // Chunks finish out of order across threads, so progress is the count of
            // finished chunks, not the offset of the one this thread just played.
            uint64_t done = doneChunks.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done % PRINT_CHUNKS == 0 || done == chunks) {
                report();
            }
        }
    }

    void report() {
        // The count is read under the lock, so successive prints never go backwards.
        std::lock_guard<std::mutex> lock(printMutex);
        const uint64_t played = std::min(
            doneChunks.load(std::memory_order_relaxed) * CHUNK_HANDS, EPOCHS);
        std::ostringstream out;
        out << std::fixed << std::setprecision(3);
        out << "Training: " << 100.0 * double(played) / double(EPOCHS) << "% Complete\n";
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

    void run() {
        lastReportTime = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned t = 0; t < threads; t++) {
            pool.emplace_back([this, t] {
                try {
                    Worker worker(t);
                    runTraining(worker);
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
        const uint64_t pruned = strategy.compact();
        std::cout << "Pruned " << pruned << " nodes under " << VISITS_FLOOR << " visits" << std::endl;
    }

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
                uint64_t variances;
                std::memcpy(&key, cursor, sizeof(key));
                std::memcpy(weights.data(), cursor + 8, sizeof(weights));
                std::memcpy(&variances, cursor + 28, sizeof(variances));
                strategyBase.insert(key, weights, variances);
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
                out[j] = float(loadDouble(weights.weights[j]));
            }
            uint32_t visits = uint32_t(std::min<uint64_t>(entryVisits(weights), UINT32_MAX));
            uint64_t key = weights.key.load(std::memory_order_relaxed);
            uint64_t variances = weights.variances.load(std::memory_order_relaxed);
            writeFile.write(&key, sizeof(key));
            writeFile.write(&out, sizeof(out));
            writeFile.write(&visits, sizeof(visits));
            writeFile.write(&variances, sizeof(variances));
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
                  << "LEARNING_RATE: " << LEARNING_RATE << "\n"
                  << "VISITS_FLOOR: " << VISITS_FLOOR << std::endl;

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
