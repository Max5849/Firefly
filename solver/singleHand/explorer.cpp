#include "solver/singleHand/config.h"
#include "solver/singleHand/engine.h"
#include "solver/singleHand/strategyTable.h"
#include "solver/singleHand/baseTable.h"
#include "solver/singleHand/trace.h"

double MIN_DELTA_INC;
double DELTA_SCALAR;
double INV_MIN_DELTA_INC;
double INV_DELTA_SCALAR;

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

// One decision of the hand: the acting seat's hand looked up its one node.
struct Decision {
    Entry* entry;
    int seat;
    int option;
    float chips;
    float pot;
    // The seat's ratio just before the decision: the expected weight of the touch given
    // the hand reached it.
    double ratio;
};

struct Worker {
    Engine engine;
    std::mt19937 gen;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    std::vector<Decision> decisions;
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
    std::mutex printMutex;

    explicit Explorer(unsigned threads) : threads(threads), bar(threads), nextChunk(0), doneChunks(0) {}

    // Finds the node for one key, inserting it if new. Visits are not counted here: they
    // are counted at the end of the hand, weighted by the ratio.
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

    // Draws the acting seat's action from its node's sampling policy.
    //
    // Every seat still in the hand, the acting one included, multiplies its ratio by the
    // option's INV_TEMP probability over its sampled probability, so its weights count
    // the line at the frequency the policy really plays it rather than the softer one it
    // was drawn at. This is the range trainer's reach / odds for a range of one combo.
    int chooseOption(Worker& worker) {
        Engine& engine = worker.engine;
        const int seat = engine.getAction();
        // setActionState() wrote spr and cpr, which getOptions() reads.
        const std::array<bool, 5> options = engine.getOptions();
        const uint64_t key = engine.getActionState(MEMORY_BASIC)
            | engine.getMetaState(METAS_BASIC) << 42;
        Entry& weights = locateNode(engine, key, options);
        std::array<double, 5> policy;
        std::array<double, 5> sampling;
        nodePolicy(weights, policy, sampling);

        double running = 0;
        std::array<double, 5> dividers;
        for (int j = 0; j < 5; j++) {
            running += sampling[j];
            dividers[j] = running;
        }
        int option = 0;
        double sample = worker.unit(worker.gen) * running;
        for (int j = 0; j < 5; j++) {
            if (sample < dividers[j] && sampling[j] > 0) {
                option = j;
                break;
            }
        }

        const int decisionIndex = int(worker.decisions.size());
        worker.decisions.push_back({&weights, seat, option, engine.getChips(),
            engine.getPot(), engine.getRatio(seat)});
        const double ratio = policy[option] / sampling[option];
        for (int other = 0; other < PLAYER_COUNT; other++) {
            if (engine.players[other].inHand) {
                engine.getRatio(other) *= ratio;
            }
        }

        if constexpr (TRACE_KEY_ENABLED) {
            if (key == TRACE_KEY) {
                const uint64_t stats = weights.stats.load(std::memory_order_relaxed);
                TraceNode node{};
                node.key = key;
                node.decision = decisionIndex;
                node.combo = engine.getHoleCombo(seat);
                node.action = seat;
                node.option = option;
                node.legal = entryOptions(stats);
                node.visits = entryVisits(stats);
                node.delta = entryDelta(stats);
                node.ratio = worker.decisions.back().ratio;
                for (int j = 0; j < 4; j++) {
                    node.weights[j] = std::bit_cast<double>(
                        weights.weights[j].load(std::memory_order_relaxed));
                }
                node.probabilities = policy;
                node.sampled = sampling;
                node.chips = worker.decisions.back().chips;
                node.pot = worker.decisions.back().pot;
                node.updated = false;
                worker.tracePath.push_back(node);
            }
        }
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

    // At the end of the hand every decision takes one EMA step toward its seat's pot
    // normalised return, scaled by the seat's final ratio: the line was drawn at the
    // sampled frequencies of every seat, so the ratio weights the touch by how often it
    // would really have happened.
    //
    // Visits add each decision's expected weight given the hand reached it - the ratio
    // just before it - rather than the weight itself: everything from the decision on
    // averages to 1, so the expectation drops that noise. Folds count too. In 1/256
    // units with stochastic rounding.
    void updateWeights(Worker& worker, double multiplier) {
        Engine& engine = worker.engine;
        const std::array<float, PLAYER_COUNT> rewards = engine.getRewards();
        for (size_t d = 0; d < worker.decisions.size(); d++) {
            const Decision& decision = worker.decisions[d];
            Entry& weights = *decision.entry;
            const double mass = 256.0 * decision.ratio;
            if (mass > 0.0) {
                uint64_t add = uint64_t(mass);
                add += worker.unit(worker.gen) < mass - double(add);
                if (add > 0) {
                    addVisits(weights, add);
                }
            }
            const double ratio = engine.getRatio(decision.seat);
            if (decision.option == 0 || multiplier == 0.0 || !(ratio > 0.0)) {
                continue;
            }
            const double step = LEARNING_RATE * multiplier * MIN_DELTA_INC
                * std::exp(DELTA_SCALAR * entryDelta(weights.stats.load(std::memory_order_relaxed)));
            // A touch of weight w is w unit steps: it keeps exp(-rate * w).
            const double s = -std::expm1(-step * ratio);
            const double target = (rewards[decision.seat] - decision.chips) / decision.pot;
            auto [before, after] = affineUpdate(weights.weights[decision.option - 1],
                1.0 - s, s * target);
            if constexpr (TRACE_KEY_ENABLED) {
                for (TraceNode& node : worker.tracePath) {
                    if (node.decision != int(d)) {
                        continue;
                    }
                    node.updated = true;
                    node.step = s;
                    node.target = target;
                    node.before = before;
                    node.after = after;
                }
            }
        }
        if constexpr (TRACE_KEY_ENABLED) {
            for (TraceNode& node : worker.tracePath) {
                node.finalRatio = engine.getRatio(node.action);
            }
            printTrace(worker);
        }
        worker.decisions.clear();
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
        worker.decisions.clear();
        while (true) {
            do {
                if (!MEMORY_BASIC || (USE_BASE && !BASE_MEMORY_BASIC)) {
                    engine.setActionState(false);
                }
                if (MEMORY_BASIC || (USE_BASE && BASE_MEMORY_BASIC)) {
                    engine.setActionState(true);
                }
            } while (engine.play(chooseOption(worker)));
            if (!engine.beginNewRound()) {
                engine.showdown();
                break;
            }
            engine.setMetaState(!METAS_BASIC || (USE_BASE && !BASE_METAS_BASIC),
                METAS_BASIC || (USE_BASE && BASE_METAS_BASIC));
        }
        updateWeights(worker, multiplier);
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
        std::cout << out.str() << std::flush;
    }

    void workerMain(unsigned t) {
        Worker worker(t);
        runBlock(worker, Phase::Warmup, 0, WARMUP_HANDS);
        for (uint64_t block = 0; block < BLOCKS; block++) {
            deltaPass(t);
            runBlock(worker, Phase::Training, block * BLOCK, BLOCK);
        }
        deltaPass(t);
        runBlock(worker, Phase::Cooldown, 0, COOLDOWN_HANDS);
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

    // The last digit names the trainer: 0 for this single hand trainer, 1 for range.
    static std::string spec(bool metasBasic, bool memoryBasic, bool ranged) {
        return std::to_string((int)(100000 * PLAYER_COUNT + 100 * CHIP_COUNT
            + 100 * metasBasic + 10 * memoryBasic + ranged));
    }

    void downloadStrategyBase() {
        std::string path = "solver/strategies/strategy_"
            + spec(BASE_METAS_BASIC, BASE_MEMORY_BASIC, BASE_RANGED) + ".bin";
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
            + spec(METAS_BASIC, MEMORY_BASIC, RANGED) + ".bin");
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
