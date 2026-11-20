// Prunes a strategy file to the keys reachable through frequent actions:
//
//   build/compress <threshold> [solver/strategies/strategy_<spec>.bin] [--seed N] [--max-deals N]
//
// A key is kept when some path from the deal reaches it on which every action, at every
// seat, has policy frequency >= threshold. The survivors are written, in their original
// order and byte for byte, to strategy_<spec number><COMPRESS_TAG>.bin (config.h) beside
// the input. That name must not exist yet, and the input is never modified. With no path,
// the file config.h names is read.
//
// The file stores no edges: a key holds no cards and only buckets of the stacks, so it
// does not determine its children. The tree is walked by replaying hands through
// Engine, reading each decision's policy from the file and following every option at or
// above the threshold. A key with many parents is kept if any of them reaches it; only a
// reached bit per record is stored. Chance cannot be enumerated, so deals are sampled,
// each one's above-threshold action tree expanded in full, until a window of deals finds
// fewer than COMPRESS_MIN_NEW_RATE new keys per deal (or --max-deals, by default
// COMPRESS_MAX_DEALS). A key
// reachable only on very rare deals can be missed, and so cut.
//
// Legality is recomputed with getOptions(): a file trained with USE_BASE's prune stored
// nothing about it, so its policies here would differ slightly from training's.
//
// Memory: a read only mapping of the file (page cache), a 64-bit slot index at a load of
// at most COMPRESS_MAX_LOAD (16 GB for a full 0.95 * 2^30 record file), and one bit per
// record. Build from the repo root:
//
//   clang++ -std=c++2b -O2 -I. -o build/compress solver/trainer/compress.cpp

#include "solver/trainer/config.h"
#include "solver/trainer/engine.h"
#include "solver/trainer/strategyTable.h"

#include <fcntl.h>
#include <unistd.h>

// Deals a worker claims at a time.
constexpr uint64_t COMPRESS_CHUNK_DEALS = 1ULL << 16;
// Deals between progress reports, and the window the stopping rule measures.
constexpr uint64_t COMPRESS_WINDOW_DEALS = 1ULL << 20;
// Stop once a window finds fewer new keys than this per deal.
constexpr double COMPRESS_MIN_NEW_RATE = 1e-6;
// Stop after this many deals whatever the rate (the training run's hand count), unless
// --max-deals says otherwise.
constexpr uint64_t COMPRESS_MAX_DEALS = EPOCHS;
// Decisions one deal may expand before its tree is cut short (counted as truncated).
constexpr uint64_t COMPRESS_MAX_NODES_PER_DEAL = 1ULL << 32;
// The index's largest load factor.
constexpr double COMPRESS_MAX_LOAD = 0.8;

constexpr uint32_t COMPRESS_METAS_MASK = 1u << METAS_STAGE;

// A read only mapping of the strategy file.
struct MappedStrategy {
    const uint8_t* data = nullptr;
    uint64_t bytes = 0;
    uint64_t count = 0;

    void open(const std::string& path) {
        bytes = fileSize(path);
        if (bytes < STRATEGY_HEADER_BYTES) {
            throw std::runtime_error(path + " is truncated");
        }
        {
            std::ifstream file = openRead(path);
            readExact(file, &count, sizeof(count), path);
        }
        if (bytes != STRATEGY_HEADER_BYTES + count * STRATEGY_RECORD_BYTES) {
            throw std::runtime_error(path + " is not " + std::to_string(count) + " records of "
                + std::to_string(STRATEGY_RECORD_BYTES) + " bytes");
        }
        if (count >= 0xFFFFFFFFULL) {
            throw std::runtime_error(path + " has too many records for a 32 bit index");
        }
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("failed to open " + path + " for mapping");
        }
        void* address = ::mmap(nullptr, size_t(bytes), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (address == MAP_FAILED) {
            throw std::runtime_error("failed to map " + path);
        }
        data = static_cast<const uint8_t*>(address);
    }

    void advise(int advice) const {
        ::madvise(const_cast<uint8_t*>(data), size_t(bytes), advice);
    }

    const uint8_t* record(uint64_t index) const {
        return data + STRATEGY_HEADER_BYTES + index * STRATEGY_RECORD_BYTES;
    }

    uint64_t key(uint64_t index) const {
        uint64_t key;
        std::memcpy(&key, record(index), sizeof(key));
        return key;
    }

    ~MappedStrategy() {
        if (data != nullptr) {
            ::munmap(const_cast<uint8_t*>(data), size_t(bytes));
        }
    }
};

// Open addressed key -> record index over the mapping. A slot is 0 (empty) or
// (tag << 32) | (index + 1), the tag being mix64's low 32 bits with bit 31 set, as in
// StrategyTable, so a probe reads the file only on a tag match.
struct StrategyIndex {
    std::atomic<uint64_t>* slots = nullptr;
    uint64_t slotBits = 0;
    uint64_t mask = 0;
    const MappedStrategy* file = nullptr;

    static uint64_t tagOf(uint64_t hash) { return (hash & 0xFFFFFFFFULL) | 0x80000000ULL; }

    void build(const MappedStrategy& strategy, unsigned threads, std::atomic<uint64_t>& duplicates) {
        file = &strategy;
        slotBits = 4;
        while (double(strategy.count) > COMPRESS_MAX_LOAD * double(1ULL << slotBits)) {
            slotBits++;
        }
        mask = (1ULL << slotBits) - 1;
        void* p = ::mmap(nullptr, size_t(1ULL << slotBits) * sizeof(uint64_t),
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            throw std::runtime_error("mmap failed for the index");
        }
        slots = reinterpret_cast<std::atomic<uint64_t>*>(p);

        strategy.advise(MADV_SEQUENTIAL);
        std::vector<std::thread> pool;
        const uint64_t per = (strategy.count + threads - 1) / threads;
        for (unsigned t = 0; t < threads; t++) {
            pool.emplace_back([&, t] {
                const uint64_t begin = std::min(strategy.count, per * t);
                const uint64_t end = std::min(strategy.count, begin + per);
                for (uint64_t r = begin; r < end; r++) {
                    if (!insert(strategy.key(r), r)) {
                        duplicates.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& thread : pool) {
            thread.join();
        }
        strategy.advise(MADV_RANDOM);
    }

    // False when the key is already indexed (the first insert keeps the slot).
    bool insert(uint64_t key, uint64_t index) {
        const uint64_t hash = mix64(key);
        const uint64_t word = tagOf(hash) << 32 | (index + 1);
        for (uint64_t i = hash >> (64 - slotBits); ; i = (i + 1) & mask) {
            uint64_t current = slots[i].load(std::memory_order_relaxed);
            while (current == 0) {
                if (slots[i].compare_exchange_weak(current, word, std::memory_order_relaxed)) {
                    return true;
                }
            }
            if ((current >> 32) == (word >> 32)
                    && file->key((current & 0xFFFFFFFFULL) - 1) == key) {
                return false;
            }
        }
    }

    // The record index, or -1 when the file does not hold the key.
    int64_t find(uint64_t key) const {
        const uint64_t hash = mix64(key);
        const uint64_t tag = tagOf(hash);
        for (uint64_t i = hash >> (64 - slotBits); ; i = (i + 1) & mask) {
            const uint64_t current = slots[i].load(std::memory_order_relaxed);
            if (current == 0) {
                return -1;
            }
            if ((current >> 32) == tag) {
                const uint64_t index = (current & 0xFFFFFFFFULL) - 1;
                if (file->key(index) == key) {
                    return int64_t(index);
                }
            }
        }
    }

    ~StrategyIndex() {
        if (slots != nullptr) {
            ::munmap(slots, size_t(1ULL << slotBits) * sizeof(uint64_t));
        }
    }
};

struct DealStats {
    uint64_t nodes = 0;
    uint64_t newKeys = 0;
    uint64_t misses = 0;
    bool truncated = false;
};

struct Compressor {
    const MappedStrategy& strategy;
    const StrategyIndex& index;
    const double threshold;
    const unsigned threads;
    const uint64_t seed;
    const uint64_t maxDeals;
    // One bit per record: reached by an above-threshold path.
    std::vector<std::atomic<uint64_t>> reached;

    std::atomic<uint64_t> nextChunk{0};
    std::atomic<bool> stop{false};
    alignas(128) std::atomic<uint64_t> doneDeals{0};
    alignas(128) std::atomic<uint64_t> totalNodes{0};
    std::atomic<uint64_t> totalNew{0};
    std::atomic<uint64_t> totalMisses{0};
    std::atomic<uint64_t> totalTruncated{0};
    std::atomic<uint64_t> maxNodes{0};
    alignas(128) std::mutex printMutex;
    // Guarded by printMutex: the counts at the last report.
    uint64_t lastDeals = 0;
    uint64_t lastNew = 0;
    std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();

    Compressor(const MappedStrategy& strategy, const StrategyIndex& index, double threshold,
            unsigned threads, uint64_t seed, uint64_t maxDeals)
        : strategy(strategy), index(index), threshold(threshold), threads(threads), seed(seed),
          maxDeals(maxDeals), reached((strategy.count + 63) / 64) {}

    uint64_t keptCount() const { return totalNew.load(std::memory_order_relaxed); }

    // Plays option and runs the hand on to its next decision, as Explorer::advance does
    // but with no showdown: payouts are not needed. False when the hand is over.
    static bool advance(Engine& engine, int option) {
        if (engine.play(option)) {
            return true;
        }
        if (!engine.beginNewRound()) {
            return false;
        }
        engine.setMetaState(COMPRESS_METAS_MASK);
        return true;
    }

    // Marks the key at this decision and expands every option at or above the threshold.
    void expand(Engine& engine, DealStats& stats) {
        if (stats.nodes >= COMPRESS_MAX_NODES_PER_DEAL) {
            stats.truncated = true;
            return;
        }
        stats.nodes++;
        engine.setActionState(MEMORY_STAGE);
        // setActionState() wrote spr and cpr, which getOptions() reads.
        const std::array<bool, 5> options = engine.getOptions();
        const uint64_t key = engine.getActionState(MEMORY_STAGE)
            | engine.getMetaState(METAS_STAGE) << ACTION_STATE_BITS;
        const int64_t found = index.find(key);
        if (found < 0) {
            // Training never kept this state, so it has no policy to follow.
            stats.misses++;
            return;
        }
        const uint64_t bit = 1ULL << (uint64_t(found) & 63);
        if (!(reached[uint64_t(found) >> 6].fetch_or(bit, std::memory_order_relaxed) & bit)) {
            stats.newKeys++;
        }

        // The policy as Explorer::nodePolicy and the server compute it: fold's weight is
        // the exact 0, the others the file's codes.
        std::array<int16_t, 4> codes;
        std::memcpy(codes.data(), strategy.record(uint64_t(found)) + 8, sizeof(codes));
        const double temperature = invTemp(engine.round);
        std::array<double, 5> policy;
        double sum = 0;
        for (int j = 0; j < 5; j++) {
            double draw = 0;
            if (options[j]) {
                const double w = j == 0 ? 0.0 : decodeWeight(codes[j - 1]);
                draw = std::exp(std::clamp(temperature * w, -443.0, 443.0));
            }
            policy[j] = draw;
            sum += draw;
        }
        for (int j = 0; j < 5; j++) {
            if (!options[j] || policy[j] / sum < threshold) {
                continue;
            }
            Engine branch = engine;
            if (advance(branch, j)) {
                expand(branch, stats);
            }
        }
    }

    void runWorker(unsigned t) {
        Engine engine;
        std::mt19937 gen(seed + t);
        const uint64_t chunks = (maxDeals + COMPRESS_CHUNK_DEALS - 1) / COMPRESS_CHUNK_DEALS;
        while (!stop.load(std::memory_order_relaxed)) {
            const uint64_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= chunks) {
                return;
            }
            const uint64_t deals = std::min(COMPRESS_CHUNK_DEALS,
                maxDeals - chunk * COMPRESS_CHUNK_DEALS);
            uint64_t nodes = 0, newKeys = 0, misses = 0, truncated = 0, most = 0;
            for (uint64_t d = 0; d < deals; d++) {
                engine.shuffle(gen);
                engine.deal();
                DealStats stats;
                Engine hand = engine;
                expand(hand, stats);
                nodes += stats.nodes;
                newKeys += stats.newKeys;
                misses += stats.misses;
                truncated += stats.truncated;
                most = std::max(most, stats.nodes);
            }
            totalNodes.fetch_add(nodes, std::memory_order_relaxed);
            totalNew.fetch_add(newKeys, std::memory_order_relaxed);
            totalMisses.fetch_add(misses, std::memory_order_relaxed);
            totalTruncated.fetch_add(truncated, std::memory_order_relaxed);
            uint64_t seen = maxNodes.load(std::memory_order_relaxed);
            while (most > seen && !maxNodes.compare_exchange_weak(seen, most,
                std::memory_order_relaxed)) {}
            const uint64_t before = doneDeals.fetch_add(deals, std::memory_order_relaxed);
            const uint64_t after = before + deals;
            if (before / COMPRESS_WINDOW_DEALS != after / COMPRESS_WINDOW_DEALS
                    || after >= maxDeals) {
                report();
            }
        }
    }

    // Prints progress and applies the stopping rule to the deals since the last report.
    void report() {
        std::lock_guard<std::mutex> lock(printMutex);
        const uint64_t deals = doneDeals.load(std::memory_order_relaxed);
        const uint64_t kept = totalNew.load(std::memory_order_relaxed);
        if (deals <= lastDeals) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - lastTime).count();
        const double rate = double(kept - lastNew) / double(deals - lastDeals);
        std::ostringstream out;
        out << std::fixed << std::setprecision(3)
            << "Deals: " << deals
            << "  Kept: " << kept << " (" << 100.0 * double(kept) / double(strategy.count) << "%)"
            << std::setprecision(7) << "  New/deal: " << rate
            << std::setprecision(1)
            << "  Nodes/deal: " << double(totalNodes.load(std::memory_order_relaxed)) / double(deals)
            << "  Max nodes: " << maxNodes.load(std::memory_order_relaxed)
            << "  Misses: " << totalMisses.load(std::memory_order_relaxed)
            << "  Truncated: " << totalTruncated.load(std::memory_order_relaxed)
            << std::setprecision(0) << "  Deals/s: " << double(deals - lastDeals) / std::max(seconds, 1e-9)
            << "\n";
        std::cout << out.str() << std::flush;
        if (rate < COMPRESS_MIN_NEW_RATE && !stop.load(std::memory_order_relaxed)) {
            stop.store(true, std::memory_order_relaxed);
            std::cout << "Stopping: under " << COMPRESS_MIN_NEW_RATE << " new keys per deal\n"
                      << std::flush;
        }
        lastDeals = deals;
        lastNew = kept;
        lastTime = now;
    }

    void run() {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++) {
            pool.emplace_back([this, t] {
                try {
                    runWorker(t);
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
        report();
    }

    // Copies the reached records, in file order and byte for byte, to path.
    void write(const std::string& path) {
        strategy.advise(MADV_SEQUENTIAL);
        uint64_t kept = 0;
        for (const auto& word : reached) {
            kept += uint64_t(std::popcount(word.load(std::memory_order_relaxed)));
        }
        CheckedWriter writer(path);
        writer.write(&kept, sizeof(kept));
        uint64_t written = 0;
        for (uint64_t r = 0; r < strategy.count; r++) {
            if (reached[r >> 6].load(std::memory_order_relaxed) >> (r & 63) & 1) {
                writer.write(strategy.record(r), STRATEGY_RECORD_BYTES);
                written++;
            }
        }
        if (written != kept) {
            throw std::runtime_error("wrote " + std::to_string(written) + " records, expected "
                + std::to_string(kept));
        }
        writer.commit();
    }
};

// strategy_<spec number><tag>.bin -> strategy_<spec number><COMPRESS_TAG>.bin in the same
// folder. Refuses a name that already exists, so neither the input nor a trained strategy
// that happens to carry COMPRESS_TAG can be overwritten.
static std::string compressedPath(const std::string& path) {
    const std::filesystem::path output = std::filesystem::path(path).parent_path()
        / ("strategy_" + trainerSpec(METAS_STAGE, MEMORY_STAGE, COMPRESS_TAG) + ".bin");
    if (std::filesystem::exists(output)) {
        throw std::runtime_error(output.string() + " already exists (the input, or an earlier "
            "output): delete it or choose another COMPRESS_TAG in config.h");
    }
    return output.string();
}

// Keys depend on the players, stack, ante and both stages, which config.h fixes at compile
// time, so a file trained at other settings would find nothing and be emptied.
static void checkSpec(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    const std::string expected = "strategy_" + trainerSpecNumber(METAS_STAGE, MEMORY_STAGE);
    if (name.rfind(expected, 0) != 0 || name.size() <= expected.size()
            || !std::isalpha(static_cast<unsigned char>(name[expected.size()]))) {
        throw std::runtime_error(name + " was not trained at config.h's settings (expected "
            + expected + "<tag>...): set PLAYER_COUNT, CHIP_COUNT, ANTE, METAS_STAGE and "
            "MEMORY_STAGE to match it and rebuild");
    }
}

int main(int argc, char** argv) {
    try {
        double threshold = -1;
        std::string path;
        uint64_t seed = 15;
        uint64_t maxDeals = COMPRESS_MAX_DEALS;
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "--seed") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("--seed needs a value");
                }
                seed = std::stoull(argv[++i]);
            } else if (arg == "--max-deals") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("--max-deals needs a value");
                }
                maxDeals = std::stoull(argv[++i]);
                if (maxDeals == 0) {
                    throw std::runtime_error("--max-deals must be positive");
                }
            } else if (threshold < 0) {
                threshold = std::stod(arg);
            } else if (path.empty()) {
                path = arg;
            } else {
                throw std::runtime_error("unrecognised argument " + arg);
            }
        }
        if (!(threshold > 0 && threshold <= 1)) {
            throw std::runtime_error(
                "usage: compress <threshold in (0, 1]> [strategy_<spec>.bin] [--seed N] [--max-deals N]");
        }
        if (path.empty()) {
            path = "solver/strategies/strategy_"
                + trainerSpec(METAS_STAGE, MEMORY_STAGE, STRATEGY_TAG) + ".bin";
        }
        checkSpec(path);
        const std::string output = compressedPath(path);

        unsigned threads = std::thread::hardware_concurrency();
        if (const char* override = std::getenv("FIREFLY_THREADS")) {
            threads = unsigned(std::strtoul(override, nullptr, 10));
        }
        threads = std::max(threads, 1u);

        std::cout << "Input: " << path << "\nOutput: " << output
                  << "\nThreshold: " << threshold << "\nThreads: " << threads
                  << "\nSeed: " << seed << "\nMax deals: " << maxDeals << std::endl;

        Engine::loadTables();
        Engine::requireStages(COMPRESS_METAS_MASK);

        MappedStrategy strategy;
        strategy.open(path);
        std::cout << "Records: " << strategy.count << std::endl;

        const auto indexStart = std::chrono::steady_clock::now();
        StrategyIndex index;
        std::atomic<uint64_t> duplicates{0};
        index.build(strategy, threads, duplicates);
        std::cout << "Index: 2^" << index.slotBits << " slots ("
                  << double(1ULL << index.slotBits) * 8 / double(1ULL << 30) << " GB) in "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - indexStart).count()
                  << " s" << std::endl;
        if (duplicates.load() > 0) {
            std::cout << "Warning: " << duplicates.load()
                      << " duplicate keys; only the first record of each can be kept" << std::endl;
        }

        Compressor compressor(strategy, index, threshold, threads, seed, maxDeals);
        compressor.run();
        compressor.write(output);

        const uint64_t kept = compressor.keptCount();
        std::cout << std::fixed << std::setprecision(3)
                  << "Kept " << kept << " of " << strategy.count << " records ("
                  << 100.0 * double(kept) / double(strategy.count) << "%), "
                  << double(fileSize(output)) / double(1ULL << 30) << " GB of "
                  << double(strategy.bytes) / double(1ULL << 30) << " GB" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
