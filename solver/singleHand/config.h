#pragma once
//shift to odds floor
#include "solver/include/library.h"
#include <cmath>

constexpr int PLAYER_COUNT = 7;
constexpr float CHIP_COUNT = 400.0;
constexpr bool METAS_BASIC = true;
constexpr bool MEMORY_BASIC = true;
constexpr bool USE_BASE = false;
constexpr bool BASE_METAS_BASIC = true;
constexpr bool BASE_MEMORY_BASIC = true;
constexpr double BASE_CONFIDENCE = 16.0;

constexpr uint64_t EPOCHS = uint64_t(4) * 131072000;
// The policy's temperature, per pot: every seat plays P(j) ∝ exp(INV_TEMP * w_j), and an
// opponent's option is drawn from it, so external sampling needs no reweighting.
constexpr double INV_TEMP = 20 * 0.69314718056;
// (E / z)^2: an option's weight is the running mean of its returns until its node has
// variance / LEARNING_RATE visits, the sample size that puts it within E pots at
// confidence z, then an EMA that holds that margin.
constexpr double LEARNING_RATE = 1.0 / 1537.0;
// A node with fewer visits at the end of the run is not saved.
constexpr double VISITS_FLOOR = 0.0;


// Prints every decision that reaches TRACE_KEY, and its update. The key is on the
// current layout: action state in bits 0-41, meta state in bits 42-63.
constexpr bool TRACE_KEY_ENABLED = false;
constexpr uint64_t TRACE_KEY = 0xfc0012000021;
// Prints one in every TRACE_EVERY traces: a hand that reaches TRACE_KEY is one trace,
// counted across all threads. 1 prints them all.
constexpr uint64_t TRACE_EVERY = 10000;

// A strategy_<spec>.bin record: uint64 key, float weights[4] for options 1..4, uint32
// visits, and uint64 variances, the four 16-bit codes packed as in Entry::variances.
// Weights are doubles only while the run holds them; every file the solver writes or
// reads carries floats.
constexpr uint64_t STRATEGY_RECORD_BYTES = 36;
constexpr uint64_t STRATEGY_HEADER_BYTES = 8;

constexpr uint64_t STRATEGY_CAP_LOG2 = 28;
constexpr double MAX_LOAD = 0.95;
// The base table carries no fingerprint array, so a miss walks the whole cluster and
// touches a 24 byte record at every step. Half the load is worth twice the slots.
constexpr double BASE_MAX_LOAD = 0.5;

constexpr uint64_t CHUNK_HANDS = 1ULL << 16;
constexpr uint64_t PRINT_CHUNKS = 1ULL << 8;

// Clamp on INV_TEMP * w before exp, so a runaway weight cannot turn the option total
// into inf or zero.
inline constexpr double MAX_EXP_ARG = 700.0;
// The least variance of pot-relative return, in pots squared, the step reads for an
// option, so an option with only a few samples, whose variance can read near 0, cannot
// take a near-full step.
constexpr double VARIANCE_MIN = 0.25;

// Every two card holding. For cards i < j the combo index is
// i * (101 - i) / 2 + j - 1, the addressing used throughout handAnalysis.
inline constexpr int COMBO_COUNT = 1326;

inline int holePairIndex(int i, int j) {
    return i * (101 - i) / 2 + j - 1;
}

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
