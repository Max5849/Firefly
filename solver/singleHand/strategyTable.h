#pragma once

#include "solver/singleHand/config.h"

inline void cpuRelax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// One stored option (call, the two raises and all in) of a node. Fold's return is the
// exact 0, so it has none. Every field is a 64 bit word and none is ever reset.
//   weight    double, the value the policy reads
//   mean      double, the Welford mean of the option's return, as a share of the pot
//             after calling
//   variance  double, the Welford variance of that return, in pots squared
//   visits    the Welford weight so far, fixed point: 48 bits whole, 16 bits fraction
// visits is bumped first, and its new total sets the mean and variance steps. Each
// field is its own atomic, so updates racing on one option can leave the three a
// sample out of step with each other.
struct OptionStats {
    std::atomic<uint64_t> weight;
    std::atomic<uint64_t> mean;
    std::atomic<uint64_t> variance;
    std::atomic<uint64_t> visits;
};

// legal is the legal-option mask in its low 8 bits, set at insert before publish.
struct Entry {
    std::atomic<uint64_t> key;
    std::atomic<uint64_t> legal;
    std::array<OptionStats, 4> options;
};
static_assert(sizeof(Entry) == 144, "Entry must stay packed at 144 bytes");

inline constexpr double VISITS_ONE = 65536.0;

inline double loadDouble(const std::atomic<uint64_t>& word) {
    return std::bit_cast<double>(word.load(std::memory_order_relaxed));
}

inline void storeDouble(std::atomic<uint64_t>& word, double value) {
    word.store(std::bit_cast<uint64_t>(value), std::memory_order_relaxed);
}

inline uint8_t entryLegal(const Entry& entry) {
    return uint8_t(entry.legal.load(std::memory_order_relaxed));
}

inline double optionVisits(const OptionStats& option) {
    return double(option.visits.load(std::memory_order_relaxed)) / VISITS_ONE;
}

// The node's whole visits: its options' together. Fold choices are not counted.
inline uint64_t entryVisits(const Entry& entry) {
    uint64_t total = 0;
    for (const OptionStats& option : entry.options) {
        total += option.visits.load(std::memory_order_relaxed);
    }
    return total >> 16;
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

    // Pulls the key's home slot word toward the cache ahead of locate(), so a batch
    // of lookups overlaps its misses instead of taking them one at a time.
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
        dst.legal.store(src.legal.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (int j = 0; j < 4; j++) {
            const OptionStats& from = src.options[j];
            OptionStats& to = dst.options[j];
            to.weight.store(from.weight.load(std::memory_order_relaxed), std::memory_order_relaxed);
            to.mean.store(from.mean.load(std::memory_order_relaxed), std::memory_order_relaxed);
            to.variance.store(from.variance.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            to.visits.store(from.visits.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }

    // Drops every node with fewer than VISITS_FLOOR visits and rebuilds the slots.
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
