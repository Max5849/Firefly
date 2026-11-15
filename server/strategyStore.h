#pragma once

#include "solver/singleHand/config.h"
#include "solver/singleHand/strategyTable.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// A record as strategy_<spec>.bin stores it: the state key, the option 1..4
// expected values, and the four options' variance codes. There is no weight for
// option 0, because folding's pot normalised return is analytically exactly zero.
// The file holds no visits and no legal options; legality is recomputed.
//
// The file carries each weight as a 16-bit code of 1/1024 pot steps (solver/singleHand's
// encodeWeight); readRecord decodes it with decodeWeight. Every code is exact as a float.
// Variances stay as the packed codes; decodeVariance (strategyTable.h) reads them. Both
// codecs are singleHand's own, so the server cannot drift from the trainer's format.
//
// The file packs these at STRATEGY_RECORD_BYTES with no padding. Never point a
// StrategyRecord* into the mapping; copy the fields out with memcpy.
struct StrategyRecord {
    uint64_t key;
    std::array<float, 4> weights;
    // Option j's 16-bit variance code in bits 16 (j - 1) to 16 (j - 1) + 15, as
    // solver/singleHand/strategyTable.h packs Entry::variances.
    uint64_t variances;
};

inline constexpr uint64_t RECORD_BYTES = STRATEGY_RECORD_BYTES;

// The index is a sidecar rather than a rewrite of the strategy file, so the
// hundreds of megabytes the solver wrote are mapped and never copied.
struct StrategyIndexHeader {
    char magic[8];
    uint64_t records;
    uint64_t slotBits;
    uint64_t recordBytes;
};
static_assert(sizeof(StrategyIndexHeader) == 32, "index header must stay packed");

inline constexpr char STRATEGY_INDEX_MAGIC[8] = {'F', 'F', 'S', 'I', 'D', 'X', '0', '1'};

// A slot word is EMPTY, or (tag << 32) | (recordIndex + 1), mirroring
// StrategyTable's scheme so a zeroed page reads as empty with no initialisation.
// The tag always has bit 31 set, so an occupied word is >= 2^63.
inline constexpr uint64_t STRATEGY_SLOT_EMPTY = 0;

inline uint32_t strategySlotTag(uint64_t hash) {
    return uint32_t(hash >> 32) | 0x80000000u;
}

// Smallest power of two holding the records at a load factor of at most 0.5.
// Halving the load is worth the slots here: a single request does ~1326 lookups,
// and the shorter probe runs keep each one to about one page of the index.
inline uint64_t strategySlotBits(uint64_t records) {
    uint64_t bits = 1;
    while ((1ULL << bits) < 2 * std::max<uint64_t>(records, 1)) {
        bits++;
    }
    return bits;
}

// Names a strategy file, as Explorer::spec does for the config it was trained at:
// 100000 * players + 100 * chips + 100 * ante + 10 * metasStage + memoryStage, then the
// tag letter. (explorer.cpp carries main(), so its spec() cannot be included.) The chips
// and ante terms overlap unless the stack is a multiple of 10, and the players digit
// unless it is under 1000, so the server accepts only those stacks.
inline std::string strategySpec(int players, int chips, int ante, int metasStage,
        int memoryStage, char tag) {
    return std::to_string(100000 * players + 100 * chips + 100 * ante
        + 10 * metasStage + memoryStage) + tag;
}

inline std::string strategyPath(const std::string& spec) {
    return "solver/strategies/strategy_" + spec + ".bin";
}

// The sidecar index sits beside the strategy file: strategy_<spec>.bin -> .idx.
inline std::string indexPathFor(const std::string& strategyPath) {
    std::string path = strategyPath;
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".bin") == 0) {
        path.resize(path.size() - 4);
    }
    return path + ".idx";
}

// Validates strategy_<spec>.bin against the current 24 byte record format and
// returns its record count. The 36 byte float one, multiHand's 28 byte one, the 44 byte
// double one and the legacy 40/56/64 byte ones all decode into plausible looking
// nonsense rather than failing, so this check is the only thing standing between a stale file and
// silently wrong answers.
inline uint64_t strategyRecordCount(const std::string& path) {
    uint64_t bytes = fileSize(path);
    if (bytes < STRATEGY_HEADER_BYTES) {
        throw std::runtime_error(path + " is truncated");
    }
    std::ifstream file = openRead(path);
    uint64_t records;
    readExact(file, &records, sizeof(records), path);
    if (records == 0) {
        throw std::runtime_error(path + " declares zero records");
    }
    if (bytes != STRATEGY_HEADER_BYTES + records * RECORD_BYTES) {
        throw std::runtime_error(path + " is not in the "
            + std::to_string(RECORD_BYTES) + " byte record format: "
            + std::to_string(records) + " records declared but "
            + std::to_string(bytes) + " bytes on disk. Regenerate it from a run of "
            "the current solver.");
    }
    if (records >= 0xFFFFFFFFULL) {
        throw std::runtime_error(path + " has too many records for a 32 bit slot index");
    }
    return records;
}

// A read only mapping plus the length needed to unmap it.
struct MappedFile {
    const uint8_t* data = nullptr;
    uint64_t bytes = 0;

    void open(const std::string& path) {
        close();
        bytes = fileSize(path);
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("failed to open " + path + " for mapping");
        }
        void* address = ::mmap(nullptr, size_t(bytes), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (address == MAP_FAILED) {
            throw std::runtime_error("failed to map " + path);
        }
        // Lookups are scattered by mix64 and the pages are faulted in on use, so
        // read ahead would only evict pages a later probe still wants.
        ::madvise(address, size_t(bytes), MADV_RANDOM);
        data = static_cast<const uint8_t*>(address);
    }

    // Faults every page in so the first requests after a restart are not waiting
    // on disk. Reads into a sink the optimiser cannot discard.
    uint64_t warm() const {
        uint64_t sink = 0;
        for (uint64_t offset = 0; offset < bytes; offset += 4096) {
            sink += data[offset];
        }
        return sink;
    }

    void close() {
        if (data != nullptr) {
            ::munmap(const_cast<uint8_t*>(data), size_t(bytes));
            data = nullptr;
            bytes = 0;
        }
    }

    ~MappedFile() { close(); }
};

// How a strategy file lays out its keys. Nothing in the file says, so the registry
// works it out when it opens one (detectKeyFormat in replay.h).
//
// Current: actionState (43 bits) | boardField (15) << 43 | cluster (6) << 58, the
//   layout solver/singleHand/config.h names.
// Bit44: the layout before 2026-11-15: the same 43-bit action state, then the acting
//   seat's calls > 1 at bit 43, boardField << 44 and a 5-bit cluster << 59. The rules
//   are otherwise today's: a seat with calls > 1 facing a bet never reaches a key now
//   (Engine::autoFold), and one checking keeps its bit.
enum class KeyFormat { Current, Bit44 };

inline const char* keyFormatName(KeyFormat format) {
    return format == KeyFormat::Current ? "current" : "bit44";
}

// A current-layout key in a file's layout. heroCalledTwice is the acting seat's
// calls > 1, which only Bit44 keeps. False when the layout cannot hold the key: a
// Bit44 cluster has 5 bits.
inline bool fileKeyFor(KeyFormat format, uint64_t key, bool heroCalledTwice, uint64_t& out) {
    if (format == KeyFormat::Current) {
        out = key;
        return true;
    }
    const uint64_t actionState = key & ((1ULL << ACTION_STATE_BITS) - 1);
    const uint64_t meta = key >> ACTION_STATE_BITS;
    const uint64_t boardField = meta & BOARD_CLASS_MASK;
    const uint64_t cluster = meta >> BOARD_CLASS_BITS;
    if (cluster > 31) {
        return false;
    }
    out = actionState | uint64_t(heroCalledTwice) << 43 | boardField << 44 | cluster << 59;
    return true;
}

// Open addressed lookup over the mapped strategy file. Both mappings grow with
// the strategy and cost page cache rather than resident memory, so a much larger
// strategy file changes nothing here but the numbers.
struct StrategyStore {
    MappedFile strategyMap;
    MappedFile indexMap;
    const uint64_t* slots = nullptr;
    uint64_t slotMask = 0;
    uint64_t records = 0;
    // Set by the registry once the store is open, before it is shared.
    KeyFormat format = KeyFormat::Current;

    void open(const std::string& strategyPath, const std::string& indexPath) {
        records = strategyRecordCount(strategyPath);
        strategyMap.open(strategyPath);
        indexMap.open(indexPath);

        if (indexMap.bytes < sizeof(StrategyIndexHeader)) {
            throw std::runtime_error(indexPath + " is truncated");
        }
        StrategyIndexHeader header;
        std::memcpy(&header, indexMap.data, sizeof(header));
        if (std::memcmp(header.magic, STRATEGY_INDEX_MAGIC, 8) != 0) {
            throw std::runtime_error(indexPath + " is not a Firefly strategy index");
        }
        if (header.recordBytes != RECORD_BYTES) {
            throw std::runtime_error(indexPath + " was built for a different record size");
        }
        // The index is built from one specific strategy file. A count mismatch
        // means they have drifted apart, and every lookup would return a record
        // belonging to some other state.
        if (header.records != records) {
            throw std::runtime_error(indexPath + " holds " + std::to_string(header.records)
                + " records but " + strategyPath + " holds " + std::to_string(records)
                + ". Rebuild the index.");
        }
        uint64_t slotCount = 1ULL << header.slotBits;
        if (indexMap.bytes != sizeof(StrategyIndexHeader) + slotCount * sizeof(uint64_t)) {
            throw std::runtime_error(indexPath + " has an unexpected size");
        }
        slots = reinterpret_cast<const uint64_t*>(indexMap.data + sizeof(StrategyIndexHeader));
        slotMask = slotCount - 1;
    }

    void readRecord(uint64_t index, StrategyRecord& out) const {
        const uint8_t* cursor = strategyMap.data + STRATEGY_HEADER_BYTES
            + index * RECORD_BYTES;
        std::memcpy(&out.key, cursor, sizeof(out.key));
        std::array<int16_t, 4> codes;
        std::memcpy(codes.data(), cursor + 8, sizeof(codes));
        for (int j = 0; j < 4; j++) {
            out.weights[j] = float(decodeWeight(codes[j]));
        }
        std::memcpy(&out.variances, cursor + 16, sizeof(out.variances));
    }

    // Looks up a key in the server's (current) layout, translated to the file's.
    // heroCalledTwice is the acting seat's calls > 1 at the node.
    bool lookup(uint64_t key, bool heroCalledTwice, StrategyRecord& out) const {
        uint64_t fileKey;
        return fileKeyFor(format, key, heroCalledTwice, fileKey) && lookupFileKey(fileKey, out);
    }

    // Looks up a key exactly as the file stores it.
    //
    // The 32 bit tag in the slot resolves almost every miss without touching the
    // record mapping at all, which is what keeps a 1326 combo sweep cheap when
    // most of its keys are states self play never reached.
    bool lookupFileKey(uint64_t key, StrategyRecord& out) const {
        uint64_t hash = mix64(key);
        uint32_t tag = strategySlotTag(hash);
        uint64_t index = hash & slotMask;
        for (;;) {
            uint64_t word = slots[index];
            if (word == STRATEGY_SLOT_EMPTY) {
                return false;
            }
            if (uint32_t(word >> 32) == tag) {
                readRecord(uint32_t(word) - 1, out);
                // The tag collides about once in two billion probes, so the key
                // still has to be checked.
                if (out.key == key) {
                    return true;
                }
            }
            index = (index + 1) & slotMask;
        }
    }

    uint64_t warm() const {
        return strategyMap.warm() + indexMap.warm();
    }

    uint64_t mappedBytes() const {
        return strategyMap.bytes + indexMap.bytes;
    }
};

// Builds the sidecar hash index for a strategy file, then reads it back through
// StrategyStore and confirms every record is reachable at its own key. Throws on a
// duplicate key, which would split one state's statistics across two records.
inline void buildIndex(const std::string& strategyPath, const std::string& indexPath,
        std::ostream* log = nullptr) {
    uint64_t records = strategyRecordCount(strategyPath);
    uint64_t slotBits = strategySlotBits(records);
    uint64_t slotCount = 1ULL << slotBits;
    if (log != nullptr) {
        *log << std::fixed << std::setprecision(3)
             << "Strategy: " << strategyPath << "\n"
             << "Records: " << records << "\n"
             << "Slots: " << slotCount << " ("
             << (slotCount * sizeof(uint64_t) / (1024.0 * 1024.0)) << " MB, load "
             << (100.0 * double(records) / double(slotCount)) << "%)" << std::endl;
    }

    std::vector<uint64_t> slots(slotCount, STRATEGY_SLOT_EMPTY);
    MappedFile strategyMap;
    strategyMap.open(strategyPath);
    auto keyAt = [&](uint64_t i) {
        uint64_t key;
        std::memcpy(&key, strategyMap.data + STRATEGY_HEADER_BYTES
            + i * RECORD_BYTES, sizeof(key));
        return key;
    };

    uint64_t probes = 0;
    for (uint64_t i = 0; i < records; i++) {
        uint64_t key = keyAt(i);
        uint64_t hash = mix64(key);
        uint64_t slot = hash & (slotCount - 1);
        for (;;) {
            probes++;
            uint64_t word = slots[slot];
            if (word == STRATEGY_SLOT_EMPTY) {
                slots[slot] = (uint64_t(strategySlotTag(hash)) << 32) | (i + 1);
                break;
            }
            if (uint32_t(word >> 32) == strategySlotTag(hash)) {
                uint64_t other = uint32_t(word) - 1;
                if (keyAt(other) == key) {
                    throw std::runtime_error("duplicate key at records "
                        + std::to_string(other) + " and " + std::to_string(i));
                }
            }
            slot = (slot + 1) & (slotCount - 1);
        }
    }
    if (log != nullptr) {
        *log << "Mean probes per insert: " << (double(probes) / double(records)) << std::endl;
    }

    StrategyIndexHeader header{};
    std::memcpy(header.magic, STRATEGY_INDEX_MAGIC, 8);
    header.records = records;
    header.slotBits = slotBits;
    header.recordBytes = RECORD_BYTES;

    CheckedWriter writeFile(indexPath);
    writeFile.write(&header, sizeof(header));
    writeFile.write(slots.data(), slots.size() * sizeof(uint64_t));
    writeFile.commit();

    StrategyStore store;
    store.open(strategyPath, indexPath);
    StrategyRecord record;
    for (uint64_t i = 0; i < records; i++) {
        uint64_t key = keyAt(i);
        if (!store.lookupFileKey(key, record) || record.key != key) {
            throw std::runtime_error("record " + std::to_string(i)
                + " is not reachable through the index it was just written to");
        }
    }
    if (log != nullptr) {
        *log << "Wrote and verified " << indexPath << " (" << records << " keys)" << std::endl;
    }
}

// The stacks a spec can name unambiguously: multiples of 10 below 1000.
inline constexpr int MIN_CHIPS = 10;
inline constexpr int MAX_CHIPS = 990;
inline constexpr int CHIPS_STEP = 10;
inline constexpr int MAX_ANTE = 9;

// What a strategy file was trained at: the settings its spec encodes. The defaults are
// solver/singleHand/config.h's.
struct StrategyConfig {
    int players = PLAYER_COUNT;
    int chips = int(CHIP_COUNT);
    int ante = int(ANTE);
    int metasStage = METAS_STAGE;
    int memoryStage = MEMORY_STAGE;
    char tag = STRATEGY_TAG;

    std::string spec() const {
        return strategySpec(players, chips, ante, metasStage, memoryStage, tag);
    }
};

inline bool validTag(char tag) {
    return (tag >= 'A' && tag <= 'Z') || (tag >= 'a' && tag <= 'z');
}

// Reads a spec back into its settings, assuming a stack that is a multiple of 10 (the
// spec is ambiguous otherwise). False for a name no valid config produces.
inline bool decodeSpec(const std::string& text, StrategyConfig& out) {
    if (text.size() != 7 || !validTag(text.back())
            || !std::all_of(text.begin(), text.end() - 1, [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    const long value = std::stol(text.substr(0, 6));
    StrategyConfig config;
    config.players = int(value / 100000);
    const long rest = value % 100000;
    config.chips = int(rest / 1000) * 10;
    config.ante = int(rest / 100 % 10);
    config.metasStage = int(rest / 10 % 10);
    config.memoryStage = int(rest % 10);
    config.tag = text.back();
    if (config.players < 3 || config.players > 7 || config.chips < MIN_CHIPS
            || config.metasStage >= METAS_STAGE_COUNT || config.memoryStage >= MEMORY_STAGE_COUNT) {
        return false;
    }
    out = config;
    return out.spec() == text;
}

// Every strategy_<spec>.bin in solver/strategies whose spec decodes.
inline std::vector<StrategyConfig> availableStrategies() {
    std::vector<StrategyConfig> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("solver/strategies", ec)) {
        const std::string name = entry.path().filename().string();
        const std::string prefix = "strategy_";
        if (name.size() <= prefix.size() + 4 || name.compare(0, prefix.size(), prefix) != 0
                || name.compare(name.size() - 4, 4, ".bin") != 0) {
            continue;
        }
        StrategyConfig config;
        if (decodeSpec(name.substr(prefix.size(), name.size() - prefix.size() - 4), config)) {
            found.push_back(config);
        }
    }
    std::sort(found.begin(), found.end(), [](const StrategyConfig& a, const StrategyConfig& b) {
        return a.spec() < b.spec();
    });
    return found;
}

// The strategy files a server has opened, one per spec, opened on first request. A
// missing or stale sidecar index is rebuilt once, beside the strategy file.
struct StrategyRegistry {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const StrategyStore>> stores;
    bool warm = false;
    std::ostream* log = nullptr;
    // Works out a newly opened store's KeyFormat, before it is shared. Throws when it
    // cannot (the request then fails as strategyUnreadable).
    std::function<KeyFormat(const StrategyStore&, const std::string& spec)> detectFormat;

    // nullptr when strategy_<spec>.bin does not exist. Throws when it exists but cannot
    // be read.
    std::shared_ptr<const StrategyStore> get(const std::string& spec) {
        std::lock_guard<std::mutex> lock(mutex);
        auto at = stores.find(spec);
        if (at != stores.end()) {
            return at->second;
        }
        const std::string path = strategyPath(spec);
        if (!std::filesystem::exists(path)) {
            return nullptr;
        }
        const std::string index = indexPathFor(path);
        auto store = std::make_shared<StrategyStore>();
        try {
            store->open(path, index);
        } catch (const std::exception& e) {
            if (log != nullptr) {
                *log << "Index for " << path << " unusable (" << e.what() << "), rebuilding"
                     << std::endl;
            }
            store = std::make_shared<StrategyStore>();
            buildIndex(path, index, log);
            store->open(path, index);
        }
        if (detectFormat) {
            store->format = detectFormat(*store, spec);
        }
        if (warm) {
            store->warm();
        }
        if (log != nullptr) {
            *log << "Opened " << path << " (" << store->records << " records, "
                 << keyFormatName(store->format) << " key layout)" << std::endl;
        }
        stores.emplace(spec, store);
        return store;
    }

    struct Loaded {
        std::string spec;
        uint64_t records;
        KeyFormat format;
    };

    std::vector<Loaded> loaded() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Loaded> out;
        for (const auto& [spec, store] : stores) {
            out.push_back({spec, store->records, store->format});
        }
        return out;
    }
};
