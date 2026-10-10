#pragma once

#include "solver/config.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmath>
#include <string>

// A record as strategy_<spec>.bin stores it: the state key, the option 1..4
// expected values, and the visit count from the cooldown block. There is no
// weight for option 0, because folding's pot normalised return is analytically
// exactly zero.
//
// The solver holds the weights as doubles while it trains and narrows them to
// float on the way out, so the file is float and nothing here needs more.
//
// The file packs these at STRATEGY_RECORD_BYTES with no padding, so records after
// the first are not 8 byte aligned. Never point a StrategyRecord* into the
// mapping; copy the fields out with memcpy.
struct StrategyRecord {
    uint64_t key;
    std::array<float, 4> weights;
    uint32_t visits;
};

// STRATEGY_RECORD_BYTES and STRATEGY_HEADER_BYTES come from solver/config.h, so
// the writer and every reader move together.

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

// Mirrors Explorer::spec in explorer.cpp, which cannot be included here because
// that translation unit carries main(). The spec pins the player count, the stack
// depth and the abstraction, so a strategy file only answers for the config that
// produced it.
inline std::string fireflySpec() {
    return std::to_string(int(100000 * PLAYER_COUNT + 100 * CHIP_COUNT
        + 100 * METAS_BASIC + 10 * MEMORY_BASIC + RANGED));
}

inline std::string defaultStrategyPath() {
    return "solver/strategies/strategy_" + fireflySpec() + ".bin";
}

inline std::string defaultIndexPath() {
    return "solver/strategies/strategy_" + fireflySpec() + ".idx";
}

// Validates strategy_<spec>.bin against the current 28 byte record format and
// returns its record count. The 44 byte double layout, the pre-visits 40 byte one
// and the legacy 56/64 byte ones all decode into plausible looking nonsense rather
// than failing, so this check is the only thing standing between a stale file and
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
    if (bytes != STRATEGY_HEADER_BYTES + records * STRATEGY_RECORD_BYTES) {
        throw std::runtime_error(path + " is not in the "
            + std::to_string(STRATEGY_RECORD_BYTES) + " byte record format: "
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

// Open addressed lookup over the mapped strategy file. Both mappings grow with
// the strategy and cost page cache rather than resident memory, so a much larger
// strategy file changes nothing here but the numbers.
struct StrategyStore {
    MappedFile strategyMap;
    MappedFile indexMap;
    const uint64_t* slots = nullptr;
    uint64_t slotMask = 0;
    uint64_t records = 0;

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
        if (header.recordBytes != STRATEGY_RECORD_BYTES) {
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
            + index * STRATEGY_RECORD_BYTES;
        std::memcpy(&out.key, cursor, sizeof(out.key));
        std::memcpy(out.weights.data(), cursor + 8, sizeof(float) * 4);
        std::memcpy(&out.visits, cursor + 24, sizeof(out.visits));
    }

    // The 32 bit tag in the slot resolves almost every miss without touching the
    // record mapping at all, which is what keeps a 1326 combo sweep cheap when
    // most of its keys are states self play never reached.
    bool lookup(uint64_t key, StrategyRecord& out) const {
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
