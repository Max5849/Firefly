#pragma once

#include "solver/include/library.h"

#include <cmath>

constexpr int PLAYER_COUNT = 3;
constexpr float CHIP_COUNT = 100.0;
constexpr bool METAS_BASIC = true;
constexpr bool MEMORY_BASIC = true;
constexpr bool BASE_METAS_BASIC = true;
constexpr bool BASE_MEMORY_BASIC = true;
constexpr bool USE_BASE = false;
constexpr double BASE_CONFIDENCE = 16.0;
constexpr uint64_t EPOCHS = uint64_t(19) * 131072000;
constexpr double INV_TEMP = 8 * 0.69314718056;
constexpr double SAMPLE_INV_TEMP = 2 * 0.69314718056;
constexpr double FLEXIBILITY = 1.0;
constexpr double LEARNING_RATE = 1.0;
constexpr double VISITS_FLOOR = 5.0;
constexpr double VISITS_CEIL = 4000317440.0;
constexpr double RANGE_FLOOR = 0.00003771;


constexpr bool USE_BLOCKERS = true;

constexpr bool TRACE_KEY_ENABLED = false;
constexpr uint64_t TRACE_KEY = 0;

constexpr uint64_t STRATEGY_RECORD_BYTES = 28;
constexpr uint64_t STRATEGY_HEADER_BYTES = 8;

constexpr uint64_t STRATEGY_CAP_LOG2 = 28;
constexpr double MAX_LOAD = 0.95;
constexpr double BASE_MAX_LOAD = 0.5;

constexpr uint64_t CHUNK_HANDS = 1ULL << 16;
constexpr uint64_t PRINT_CHUNKS = 1ULL << 4;

inline constexpr double MAX_EXP_ARG = 700.0;

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
