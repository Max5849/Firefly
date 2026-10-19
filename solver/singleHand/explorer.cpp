#include "solver/singleHand/config.h"
#include "solver/singleHand/engine.h"
#include "solver/singleHand/strategyTable.h"
#include "solver/singleHand/baseTable.h"
#include "solver/singleHand/trace.h"

// The step's 1 / variance for an option: its Welford variance, no lower than VARIANCE_MIN.
static inline double inverseVariance(const OptionStats& option) {
    return 1.0 / std::max(loadDouble(option.variance), VARIANCE_MIN);
}

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
    // The chosen option's INV_TEMP probability over its sampled probability.
    double own;
    // exp(-H) of the node's INV_TEMP policy: 1 when one option is certain, 1 / n when n
    // are equally likely.
    double certainty;
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
                OptionStats& option = weights.options[j];
                storeDouble(option.weight, base != nullptr ? double(base->weights[j]) : 0.0);
                storeDouble(option.mean, 0.0);
                storeDouble(option.variance, 0.0);
                option.visits.store(0, std::memory_order_relaxed);
            }
            weights.legal.store(legal, std::memory_order_relaxed);
            strategy.publish(slotIndex, word);
        }
        return weights;
    }

    // The node's policy at INV_TEMP, and its sampling policy at SAMPLE_TEMP, over its
    // legal options. Fold's weight is the exact 0, so it draws exp(0) = 1 in both.
    static void nodePolicy(const Entry& weights, std::array<double, 5>& probabilities,
            std::array<double, 5>& sampling) {
        const uint8_t legal = entryLegal(weights);
        double sum = 0;
        double sampleSum = 0;
        for (int j = 0; j < 5; j++) {
            double draw = 0;
            double sampleDraw = 0;
            if (legal & (1u << j)) {
                double w = j == 0 ? 0.0 : loadDouble(weights.options[j - 1].weight);
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

        double negativeEntropy = 0;
        for (int j = 0; j < 5; j++) {
            if (policy[j] > 0) {
                negativeEntropy += policy[j] * std::log(policy[j]);
            }
        }
        const double ratio = policy[option] / sampling[option];
        const int decisionIndex = int(worker.decisions.size());
        worker.decisions.push_back({&weights, seat, option, engine.getChips(),
            engine.getPot(), engine.getRatio(seat), ratio,
            std::exp(negativeEntropy)});
        for (int other = 0; other < PLAYER_COUNT; other++) {
            if (engine.players[other].inHand) {
                engine.getRatio(other) *= ratio;
            }
        }

        if constexpr (TRACE_KEY_ENABLED) {
            if (key == TRACE_KEY) {
                TraceNode node{};
                node.key = key;
                node.decision = decisionIndex;
                node.combo = engine.getHoleCombo(seat);
                node.action = seat;
                node.option = option;
                node.legal = entryLegal(weights);
                for (int j = 0; j < 4; j++) {
                    const OptionStats& stats = weights.options[j];
                    node.visits[j] = optionVisits(stats);
                    node.means[j] = loadDouble(stats.mean);
                    node.variances[j] = loadDouble(stats.variance);
                }
                node.ratio = worker.decisions.back().ratio;
                node.certainty = worker.decisions.back().certainty;
                for (int j = 0; j < 4; j++) {
                    node.weights[j] = loadDouble(weights.options[j].weight);
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

    // Walked backward, each seat keeps a running product of the later decisions' ratios,
    // by any seat, up to and including its own fold: after the fold nothing changes its
    // return.
    //
    // The ratio of a decision is the one before it times the later decisions' own
    // ratios, leaving out its own: the option's mean is its return given it was chosen.
    // Every other decision reweights the line from SAMPLE_INV_TEMP to INV_TEMP.
    //
    // The chosen option takes one weighted Welford step of its pot-relative return, with the
    // ratio as the weight, in 1/65536 visit units with stochastic rounding. If that
    // rounds to nothing, nothing moves.
    //
    // Past warmup the weight then takes an EMA step toward the return times the hand's
    // certainty, the product over every decision in it, so every node of the hand takes
    // the same factor: a hand whose play stays undecided pulls its weights short of their
    // returns, and only reaches them as that play firms up. The step is scaled by the
    // same ratio, so neither its INV_TEMP nor its sampled probability shrinks or grows
    // it, and by 1 / variance of the option's return, read before this sample. The Welford
    // mean and variance take the raw return: they describe the return itself.
    void updateWeights(Worker& worker, bool moveWeights) {
        Engine& engine = worker.engine;
        const std::array<float, PLAYER_COUNT> rewards = engine.getRewards();
        double certainty = 1.0;
        for (const Decision& decision : worker.decisions) {
            certainty *= decision.certainty;
        }
        std::array<double, PLAYER_COUNT> onwardRatio;
        onwardRatio.fill(1.0);
        for (size_t d = worker.decisions.size(); d-- > 0; ) {
            const Decision& decision = worker.decisions[d];
            if (decision.option == 0) {
                onwardRatio[decision.seat] = 1.0;
            }
            const double ratio = decision.ratio * onwardRatio[decision.seat];
            for (int seat = 0; seat < PLAYER_COUNT; seat++) {
                onwardRatio[seat] *= decision.own;
            }
            if (decision.option == 0 || !(ratio > 0.0)) {
                continue;
            }
            OptionStats& option = decision.entry->options[decision.option - 1];
            // The return from the decision on, as a share of the pot after calling.
            const double target = (rewards[decision.seat] - decision.chips) / decision.pot;
            const double inverse = inverseVariance(option);
            const double mass = VISITS_ONE * ratio;
            uint64_t add = uint64_t(mass);
            add += worker.unit(worker.gen) < mass - double(add);
            if (add == 0) {
                continue;
            }
            const uint64_t visits = option.visits.fetch_add(add, std::memory_order_relaxed) + add;
            const double share = double(add) / double(visits);
            auto [meanBefore, meanAfter] = affineUpdate(option.mean, 1.0 - share, share * target);
            affineUpdate(option.variance, 1.0 - share,
                share * (target - meanBefore) * (target - meanAfter));
            if (!moveWeights) {
                continue;
            }
            // A touch of weight w is w unit steps: it keeps exp(-rate * w).
            const double s = -std::expm1(-LEARNING_RATE * inverse * ratio);
            const double weightTarget = certainty * target;
            auto [before, after] = affineUpdate(option.weight, 1.0 - s, s * weightTarget);
            if constexpr (TRACE_KEY_ENABLED) {
                for (TraceNode& node : worker.tracePath) {
                    if (node.decision != int(d)) {
                        continue;
                    }
                    node.updated = true;
                    node.stepRatio = ratio;
                    node.onward = certainty;
                    node.inverseVariance = inverse;
                    node.step = s;
                    node.target = target;
                    node.weightTarget = weightTarget;
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

    void playHand(Worker& worker, bool moveWeights) {
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
        updateWeights(worker, moveWeights);
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
                playHand(worker, base + j >= WARMUP_HANDS);
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
        out << (played <= WARMUP_HANDS ? "Warm up: " : "Training: ")
            << 100.0 * double(played) / double(EPOCHS) << "% Complete\n";
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

    // The trailing s marks a single hand strategy, keeping it apart from the range
    // trainer's file at the same spec.
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
                out[j] = float(loadDouble(weights.options[j].weight));
            }
            uint32_t visits = uint32_t(std::min<uint64_t>(entryVisits(weights), UINT32_MAX));
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
                  << "LEARNING_RATE: " << LEARNING_RATE << "\n"
                  << "VISITS_FLOOR: " << VISITS_FLOOR << "\n"
                  << "WARMUP_HANDS: " << WARMUP_HANDS << "\n"
                  << "VARIANCE_MIN: " << VARIANCE_MIN << std::endl;

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
