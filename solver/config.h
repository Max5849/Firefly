#pragma once

#include "solver/include/library.h"

#include <cmath>

constexpr int PLAYER_COUNT = 7;
constexpr float CHIP_COUNT = 400.0;
constexpr bool METAS_BASIC = true;
constexpr bool MEMORY_BASIC = true;
constexpr bool BASE_METAS_BASIC = true;
constexpr bool BASE_MEMORY_BASIC = true;
constexpr bool USE_BASE = false;
// Which trainer wrote a strategy file: this range trainer or the single hand one in
// solver/singleHand. The key layouts match, so a base may come from either.
constexpr bool RANGED = true;
constexpr bool BASE_RANGED = true;
constexpr uint64_t EPOCHS = 10000793600;

constexpr double INV_TEMP = 5.545177444479562;
constexpr double SAMPLE_INV_TEMP = 2.772588722239781;
constexpr double FLEXIBILITY = 1.0;
constexpr double LEARNING_RATE = 1.0;

constexpr double VISITS_FLOOR = 5.0;
constexpr double VISITS_CEIL = 4000317440.0;

// Clamp on INV_TEMP * w before exp, so a runaway weight cannot turn the option total
// into inf or zero.
inline constexpr double MAX_EXP_ARG = 700.0;

// A combo whose reach falls below this fraction of its range's summed reach is dropped:
// its reach is set to 0, so the draw, the visits, the update and the showdown all skip
// it. Relative, so a range whose reach drifts down is never emptied.
constexpr double RANGE_FLOOR = 0.00003771;

// Every two card holding. For cards i < j the combo index is
// i * (101 - i) / 2 + j - 1, the addressing used throughout handAnalysis.
inline constexpr int COMBO_COUNT = 1326;

inline int holePairIndex(int i, int j) {
    return i * (101 - i) / 2 + j - 1;
}

// A strategy_<spec>.bin record: uint64 key, float weights[4] for options 1..4, and
// uint32 visits. Weights are doubles only while the run holds them; every file the
// solver writes or reads carries floats.
constexpr uint64_t STRATEGY_RECORD_BYTES = 28;
constexpr uint64_t STRATEGY_HEADER_BYTES = 8;

// An option survives the base prune when it would still be drawn at least once in
// BASE_CONFIDENCE at INV_TEMP.
constexpr double BASE_CONFIDENCE = 16.0;
// The base table carries no fingerprint array, so a miss walks the whole cluster and
// touches a 24 byte record at every step. Half the load is worth twice the slots.
constexpr double BASE_MAX_LOAD = 0.5;

// Prints every decision that reaches TRACE_KEY, and its update. The key is on the
// current layout: action state in bits 0-41, meta state in bits 42-63.
constexpr bool TRACE_KEY_ENABLED = false;
constexpr uint64_t TRACE_KEY = 0;

constexpr uint64_t STRATEGY_CAP_LOG2 = 28;
constexpr double MAX_LOAD = 0.95;

constexpr uint64_t CHUNK_HANDS = 1ULL << 16;
constexpr uint64_t PRINT_CHUNKS = 1ULL << 4;

inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

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

inline std::vector<char> readWholeFile(const std::string& path) {
    uint64_t bytes = fileSize(path);
    std::ifstream file = openRead(path);
    std::vector<char> data(bytes);
    if (bytes > 0) {
        readExact(file, data.data(), bytes, path);
    }
    return data;
}

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
        if (file.is_open()) {
            file.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
        }
    }
};

//update skills soon
