#pragma once

#include "solver/include/library.h"

constexpr int PLAYER_COUNT = 7;
constexpr float CHIP_COUNT = 400.0;
constexpr bool METAS_BASIC = true;
constexpr bool MEMORY_BASIC = true;
constexpr bool BASE_METAS_BASIC = true;
constexpr bool BASE_MEMORY_BASIC = true;
constexpr bool USE_BASE = false;
constexpr uint64_t EPOCHS = 3 * 100 * (1ULL << 32);

// Fixed strategy table capacity. Never resized, so slot addresses are stable
// for the lifetime of the process and insertion is a single CAS.
constexpr uint64_t STRATEGY_CAP_LOG2 = 28;
// A ceiling, not an operating point: below it the load factor costs nothing, so
// this only takes effect if the run genuinely discovers that many states. At 0.95
// lookups still cost ~1.5 probes and throughput is unchanged; the cliff is ~0.99.
constexpr double MAX_LOAD = 0.95;

// Work-stealing granularity: threads claim CHUNK_HANDS hands at a time so that
// the fast performance cores naturally take more chunks than the efficiency ones.
constexpr uint64_t CHUNK_HANDS = 1ULL << 16;
// Progress is printed every PRINT_CHUNKS claimed chunks.
constexpr uint64_t PRINT_CHUNKS = 1ULL << 8;

// splitmix64 finaliser. State keys are packed bitfields whose low bits are highly
// structured, so they need a proper avalanche before indexing a table.
inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// --- checked binary file I/O -------------------------------------------------
// Every read and write below reports failure by throwing. Silence is the failure
// mode that matters here: a missing table file would otherwise leave a zeroed
// array in memory and a run would burn days producing nonsense.

inline uint64_t fileSize(const std::string& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("cannot stat " + path + ": " + ec.message());
    }
    return uint64_t(size);
}

inline std::ifstream openRead(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open " + path + " for reading");
    }
    return file;
}

inline void readExact(std::istream& file, void* destination, uint64_t bytes, const std::string& path) {
    file.read(static_cast<char*>(destination), std::streamsize(bytes));
    if (!file || uint64_t(file.gcount()) != bytes) {
        throw std::runtime_error("short read from " + path);
    }
}

// Reads a whole file into memory so record loops can be parsed with memcpy
// instead of millions of small stream reads.
inline std::vector<char> readWholeFile(const std::string& path) {
    uint64_t bytes = fileSize(path);
    std::ifstream file = openRead(path);
    std::vector<char> data(bytes);
    if (bytes > 0) {
        readExact(file, data.data(), bytes, path);
    }
    return data;
}

// Writes to a temporary alongside the destination and renames on commit, so a
// crashed, failed, or interrupted run can never destroy the previous good file.
struct CheckedWriter {
    std::string path;
    std::string temporary;
    std::vector<char> buffer;
    std::ofstream file;

    explicit CheckedWriter(std::string destination)
            : path(std::move(destination)), temporary(path + ".tmp"), buffer(1 << 20) {
        file.rdbuf()->pubsetbuf(buffer.data(), std::streamsize(buffer.size()));
        file.open(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("failed to open " + temporary + " for writing");
        }
    }

    // Stream error state is sticky, so one check in commit() catches any failure
    // anywhere in the run of writes.
    void write(const void* source, uint64_t bytes) {
        file.write(static_cast<const char*>(source), std::streamsize(bytes));
    }

    void commit() {
        file.flush();
        if (!file) {
            throw std::runtime_error("failed writing " + temporary + " (disk full?)");
        }
        file.close();
        if (!file) {
            throw std::runtime_error("failed to close " + temporary);
        }
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            throw std::runtime_error("failed to replace " + path + ": " + ec.message());
        }
    }

    ~CheckedWriter() {
        // Not committed, so something threw. Leave no partial file behind.
        if (file.is_open()) {
            file.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
        }
    }
};
