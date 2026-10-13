#pragma once

#include "solver/multiHand/config.h"

inline void cpuRelax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// stats packs the node's visits, step size and legal options into one word:
//   bits 0-39   visits in 1/256 units, saturating at 2^40 - 1 (UINT32_MAX whole visits)
//   bits 40-55  delta, the log-encoded step size
//   bits 56-63  options, the legal-option mask
// Visits are added with a CAS so the fractional carry is atomic. delta and options
// change only at insert, before publish, or at the delta pass barrier.
struct Entry {
    std::atomic<uint64_t> key;
    std::array<std::atomic<uint64_t>, 4> weights;
    std::atomic<uint64_t> stats;
};
static_assert(sizeof(Entry) == 48, "Entry must stay packed at 48 bytes");

inline constexpr uint64_t VISITS_MASK = (1ULL << 40) - 1;
inline constexpr uint64_t DELTA_MASK = 0xFFFFULL << 40;

inline uint32_t entryVisits(uint64_t stats) { return uint32_t((stats & VISITS_MASK) >> 8); }
inline uint16_t entryDelta(uint64_t stats) { return uint16_t(stats >> 40); }
inline uint8_t entryOptions(uint64_t stats) { return uint8_t(stats >> 56); }

inline uint64_t makeStats(uint8_t options, uint16_t delta) {
    return (uint64_t(options) << 56) | (uint64_t(delta) << 40);
}

// Adds add / 256 visits, saturating rather than wrapping into delta.
inline void addVisits(Entry& entry, uint64_t add) {
    uint64_t old = entry.stats.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        uint64_t visits = std::min((old & VISITS_MASK) + add, VISITS_MASK);
        next = (old & ~VISITS_MASK) | visits;
    } while (!entry.stats.compare_exchange_weak(old, next,
        std::memory_order_relaxed, std::memory_order_relaxed));
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
        for (int j = 0; j < 4; j++) {
            dst.weights[j].store(src.weights[j].load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        dst.stats.store(src.stats.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    uint64_t compact(bool keepVisits = false) {
        uint64_t n = used.load(std::memory_order_relaxed);
        uint64_t kept = 0;
        for (uint64_t i = 0; i < n; i++) {
            Entry& entry = dense[i];
            uint64_t stats = entry.stats.load(std::memory_order_relaxed);
            if (entryVisits(stats) < VISITS_FLOOR) {
                continue;
            }
            if (kept != i) {
                copyRecord(dense[kept], entry);
            }
            dense[kept].stats.store(keepVisits ? stats : stats & ~VISITS_MASK,
                std::memory_order_relaxed);
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
