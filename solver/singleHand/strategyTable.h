#pragma once

#include "solver/singleHand/config.h"

inline void cpuRelax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// stats: bits 0-4 the legal options, bit 7 seeded from the base, bits 8-63 visits.
// variances: option j's 16-bit variance code in bits 16 (j - 1) to 16 (j - 1) + 15.
// weights: options 1..4 as 6.26 fixed point (encodeFixedWeight); fold's is the exact 0.
struct Entry {
    std::atomic<uint64_t> key;
    std::atomic<uint64_t> stats;
    std::atomic<uint64_t> variances;
    std::array<std::atomic<uint32_t>, 4> weights;
};
static_assert(sizeof(Entry) == 40, "Entry must stay packed at 40 bytes");

inline constexpr double VISITS_ONE = 65536.0;
inline constexpr int VISITS_SHIFT = 8;
inline constexpr uint64_t LEGAL_MASK = 0x1F;
inline constexpr uint64_t SEEDED_BIT = 0x80;

// A variance code is log-spaced from 1/16 to 90000 pots squared: code 0 is the floor,
// code 65535 the max, so there are 65535 / log2(90000 * 16) = 3203.45 codes per octave.
// The scale is fixed rather than tied to VARIANCE_MIN, so a file reads the same whatever
// VARIANCE_MIN the run reading it has; the step applies VARIANCE_MIN when it reads a
// variance. A variance below the floor is stored as the floor, and one above the max as
// the max. Strategy files carry the codes, so these must match between a run, the base
// it reads and the server (server/strategyStore.h).
inline constexpr double VARIANCE_CODE_FLOOR = 1.0 / 16.0;
inline constexpr double VARIANCE_CODE_MAX = 90000.0;
inline constexpr double VARIANCE_CODES_PER_OCTAVE = 3203.4490972497106;
static_assert(VARIANCE_MIN >= VARIANCE_CODE_FLOOR,
    "VARIANCE_MIN is below the smallest variance a code can record");

inline const std::vector<double> varianceCodes = [] {
    std::vector<double> values(65536);
    for (int code = 0; code < 65536; code++) {
        values[code] = VARIANCE_CODE_FLOOR * std::exp2(double(code) / VARIANCE_CODES_PER_OCTAVE);
    }
    return values;
}();

// Option slot o (option o + 1) of a packed variances word.
inline double decodeVariance(uint64_t packed, int o) {
    return varianceCodes[packed >> (16 * o) & 0xFFFF];
}

// Rounds a variance to one of its two neighbouring codes, the upper with probability
// equal to how far up the gap between them it lies, so the stored value is unbiased.
// u is uniform on [0, 1).
inline uint64_t encodeVariance(double variance, double u) {
    if (!(variance > VARIANCE_CODE_FLOOR)) {
        return 0;
    }
    const double exact = VARIANCE_CODES_PER_OCTAVE * std::log2(variance / VARIANCE_CODE_FLOOR);
    if (exact >= 65535.0) {
        return 65535;
    }
    const uint64_t low = uint64_t(exact);
    const double below = varianceCodes[low];
    return low + (u * (varianceCodes[low + 1] - below) < variance - below);
}

// A strategy file's weight, in pots: a 16-bit code of 1/1024 pot steps, so it spans
// [-32, 31.999], the run's fixed point range.
inline constexpr double WEIGHT_CODES_PER_POT = 1024.0;

inline int16_t encodeWeight(double weight) {
    return int16_t(std::clamp(std::lround(weight * WEIGHT_CODES_PER_POT),
        long(INT16_MIN), long(INT16_MAX)));
}

inline double decodeWeight(int16_t code) {
    return double(code) / WEIGHT_CODES_PER_POT;
}

// A run's weight, in pots: a two's complement int32 held in a uint32, 6 integer bits and
// 26 fraction bits, so it spans [-32, 32) in steps of 2^-26. A weight outside that is
// clamped. Each write rounds to one of the two neighbouring codes, the upper with
// probability equal to how far up the gap the exact value lies, so small steps are kept
// on average rather than lost to rounding.
inline constexpr int FIXED_WEIGHT_FRACTION_BITS = 26;
inline constexpr double FIXED_WEIGHT_ONE = double(1ULL << FIXED_WEIGHT_FRACTION_BITS);
inline constexpr double FIXED_WEIGHT_MIN = double(INT32_MIN) / FIXED_WEIGHT_ONE;
inline constexpr double FIXED_WEIGHT_MAX = double(INT32_MAX) / FIXED_WEIGHT_ONE;

inline double decodeFixedWeight(uint32_t code) {
    return double(int32_t(code)) / FIXED_WEIGHT_ONE;
}

// u is uniform on [0, 1).
inline uint32_t encodeFixedWeight(double weight, double u) {
    const double exact = weight * FIXED_WEIGHT_ONE;
    if (!(exact > double(INT32_MIN))) {
        return uint32_t(INT32_MIN);
    }
    if (exact >= double(INT32_MAX)) {
        return uint32_t(INT32_MAX);
    }
    const double low = std::floor(exact);
    return uint32_t(int32_t(low) + int32_t(u < exact - low));
}

inline double loadWeight(const std::atomic<uint32_t>& word) {
    return decodeFixedWeight(word.load(std::memory_order_relaxed));
}

inline void storeWeight(std::atomic<uint32_t>& word, double value, double u) {
    word.store(encodeFixedWeight(value, u), std::memory_order_relaxed);
}

inline uint8_t entryLegal(const Entry& entry) {
    return uint8_t(entry.stats.load(std::memory_order_relaxed) & LEGAL_MASK);
}

inline bool entrySeeded(const Entry& entry) {
    return entry.stats.load(std::memory_order_relaxed) & SEEDED_BIT;
}

inline uint64_t entryVisitUnits(const Entry& entry) {
    return entry.stats.load(std::memory_order_relaxed) >> VISITS_SHIFT;
}

inline uint64_t entryVisits(const Entry& entry) {
    return entryVisitUnits(entry) >> 16;
}

inline uint64_t addVisits(Entry& entry, uint64_t add) {
    return (entry.stats.fetch_add(add << VISITS_SHIFT, std::memory_order_relaxed)
        >> VISITS_SHIFT) + add;
}

struct StrategyTable {
    static constexpr uint64_t CAP = 1ULL << STRATEGY_CAP_LOG2;
    static constexpr uint64_t MASK = CAP - 1;
    static constexpr uint64_t MAX_FILL = uint64_t(MAX_LOAD * double(CAP));
    static_assert(MAX_FILL < (1ULL << 32), "dense index must fit in 32 bits");

    static constexpr uint64_t EMPTY_WORD = 0;
    static constexpr uint64_t CLAIMED_WORD = 1;

    std::atomic<uint64_t>* slots;
    Entry* dense;
    std::atomic<uint64_t> used;

    template <class T>
    static T* mapZeroed(uint64_t n) {
        void* p = mmap(nullptr, size_t(n) * sizeof(T), PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            throw std::runtime_error("mmap failed for the strategy table");
        }
        return reinterpret_cast<T*>(p);
    }

    StrategyTable() : used(0) {
        slots = mapZeroed<std::atomic<uint64_t>>(CAP);
        dense = mapZeroed<Entry>(MAX_FILL);
    }

    uint64_t count() const { return used.load(std::memory_order_relaxed); }

    void prefetch(uint64_t key) const {
        __builtin_prefetch(&slots[mix64(key) >> (64 - STRATEGY_CAP_LOG2)]);
    }

    Entry* locate(uint64_t key, uint64_t& slotIndex, uint64_t& word, bool& inserted) {
        const uint64_t hash = mix64(key);
        const uint64_t tag = (hash & 0xFFFFFFFFULL) | 0x80000000ULL;
        uint64_t i = hash >> (64 - STRATEGY_CAP_LOG2);
        for (uint64_t probe = 0; probe < CAP; probe++, i = (i + 1) & MASK) {
            uint64_t current = slots[i].load(std::memory_order_acquire);
            for (;;) {
                if (current == CLAIMED_WORD) {
                    do {
                        cpuRelax();
                        current = slots[i].load(std::memory_order_acquire);
                    } while (current == CLAIMED_WORD);
                    continue;
                }
                if (current == EMPTY_WORD) {
                    if (used.load(std::memory_order_relaxed) >= MAX_FILL) {
                        throw std::runtime_error("strategy table exceeded MAX_LOAD");
                    }
                    uint64_t expected = EMPTY_WORD;
                    if (slots[i].compare_exchange_strong(expected, CLAIMED_WORD,
                            std::memory_order_acq_rel, std::memory_order_acquire)) {
                        uint64_t index = used.fetch_add(1, std::memory_order_relaxed);
                        if (index >= MAX_FILL) {
                            throw std::runtime_error("strategy table exceeded MAX_LOAD");
                        }
                        slotIndex = i;
                        word = (tag << 32) | (index + 1);
                        inserted = true;
                        dense[index].key.store(key, std::memory_order_relaxed);
                        return &dense[index];
                    }
                    current = expected;
                    continue;
                }
                if ((current >> 32) == tag) {
                    Entry* entry = &dense[(current & 0xFFFFFFFFULL) - 1];
                    if (entry->key.load(std::memory_order_relaxed) == key) {
                        inserted = false;
                        return entry;
                    }
                }
                break;
            }
        }
        throw std::runtime_error("strategy table full");
    }

    static void copyRecord(Entry& dst, const Entry& src) {
        dst.key.store(src.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst.stats.store(src.stats.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst.variances.store(src.variances.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (int j = 0; j < 4; j++) {
            dst.weights[j].store(src.weights[j].load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
    }

    uint64_t compact() {
        uint64_t n = used.load(std::memory_order_relaxed);
        uint64_t kept = 0;
        for (uint64_t i = 0; i < n; i++) {
            Entry& entry = dense[i];
            if (entryVisits(entry) < VISITS_FLOOR) {
                continue;
            }
            if (kept != i) {
                copyRecord(dense[kept], entry);
            }
            kept++;
        }
        used.store(kept, std::memory_order_relaxed);
        std::memset(slots, 0, CAP * sizeof(std::atomic<uint64_t>));
        for (uint64_t i = 0; i < kept; i++) {
            const uint64_t hash = mix64(dense[i].key.load(std::memory_order_relaxed));
            const uint64_t tag = (hash & 0xFFFFFFFFULL) | 0x80000000ULL;
            uint64_t s = hash >> (64 - STRATEGY_CAP_LOG2);
            while (slots[s].load(std::memory_order_relaxed) != EMPTY_WORD) {
                s = (s + 1) & MASK;
            }
            slots[s].store((tag << 32) | (i + 1), std::memory_order_relaxed);
        }
        return n - kept;
    }

    void publish(uint64_t slotIndex, uint64_t word) {
        slots[slotIndex].store(word, std::memory_order_release);
    }
};
