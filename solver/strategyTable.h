#pragma once

#include "solver/config.h"

inline void cpuRelax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

struct Entry {
    // Written before the slot is published and never touched again, so the slot's
    // release / acquire pair is all the synchronisation it needs.
    uint64_t key;
    // Option 1..4 expected values as double bit patterns, so a plain
    // compare_exchange can update one.
    std::array<std::atomic<uint64_t>, 4> weights;
    std::atomic<uint32_t> visits;
    // Step size for the whole node, derived from visits. A step size per option
    // let a rarely drawn option keep a large step and oscillate.
    std::atomic<uint8_t> delta;
    // Legal options, bit j for option j, laid out as the strategy base stores
    // them. Fixed when the record is created.
    std::atomic<uint8_t> options;
};
static_assert(sizeof(Entry) == 48, "Entry must stay packed at 48 bytes");

// A wrapped counter reports near-zero visits, which would hand a heavily visited
// node the largest step size and start it oscillating.
inline constexpr uint32_t VISITS_MAX = 0xFFFFFFFFu;

// Fixed capacity, open addressed, never resized, so record addresses stay valid
// for the whole run and insertion is a single compare_exchange.
//
// Slots are hash indexed but the records they point at are bump allocated.
// Indexing records by slot would put a live record on nearly every page even in a
// 1% full table, forcing the whole reservation resident; bump allocation keeps
// resident memory proportional to the states that exist and leaves the hot early
// states packed at the front.
struct StrategyTable {
    static constexpr uint64_t CAP = 1ULL << STRATEGY_CAP_LOG2;
    static constexpr uint64_t MASK = CAP - 1;
    static constexpr uint64_t MAX_FILL = uint64_t(MAX_LOAD * double(CAP));
    static_assert(MAX_FILL < (1ULL << 32), "dense index must fit in 32 bits");

    // A slot word is EMPTY, CLAIMED, or (tag << 32) | (denseIndex + 1). The tag
    // always has bit 31 set, so an occupied word is >= 2^63 and can never be
    // mistaken for a sentinel, which lets zeroed pages read as empty.
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

    // Both mappings are address space only; pages fault in as records are used.
    StrategyTable() : used(0) {
        slots = mapZeroed<std::atomic<uint64_t>>(CAP);
        dense = mapZeroed<Entry>(MAX_FILL);
    }

    uint64_t count() const { return used.load(std::memory_order_relaxed); }

    // Locates the record for key, allocating one if the state is new. On insertion
    // the slot is claimed but not published: the caller fills the record in, then
    // calls publish(slotIndex, word).
    Entry* locate(uint64_t key, uint64_t& slotIndex, uint64_t& word, bool& inserted) {
        const uint64_t hash = mix64(key);
        const uint64_t tag = (hash & 0xFFFFFFFFULL) | 0x80000000ULL;
        uint64_t i = hash >> (64 - STRATEGY_CAP_LOG2);
        for (uint64_t probe = 0; probe < CAP; probe++, i = (i + 1) & MASK) {
            uint64_t current = slots[i].load(std::memory_order_acquire);
            for (;;) {
                if (current == CLAIMED_WORD) {
                    // The only blocking in the design, reachable at most once per
                    // key over the whole run.
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
                        dense[index].key = key;
                        return &dense[index];
                    }
                    current = expected;
                    continue;
                }
                if ((current >> 32) == tag) {
                    Entry* entry = &dense[(current & 0xFFFFFFFFULL) - 1];
                    // The tag is the wrong record about once every two billion
                    // probes, so the key still has to be checked.
                    if (entry->key == key) {
                        inserted = false;
                        return entry;
                    }
                }
                break;
            }
        }
        throw std::runtime_error("strategy table full");
    }

    // Pairs with the acquire loads above, so a thread that sees the slot also sees
    // a fully initialised record.
    void publish(uint64_t slotIndex, uint64_t word) {
        slots[slotIndex].store(word, std::memory_order_release);
    }
};
