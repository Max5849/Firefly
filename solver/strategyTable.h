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
    // Written before the slot is published, after which the slot's release /
    // acquire pair carries it. Compaction rewrites it later from one thread with
    // every other thread parked at a barrier, so it is atomic only to say plainly
    // that more than one thread touches it; relaxed is all that is needed and on a
    // 64 bit target it is the same instruction.
    std::atomic<uint64_t> key;
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

// Visits below this in one pass carry no useful step size information, so the
// same floor decides both the delta and whether the node is worth keeping.
inline constexpr uint32_t VISIT_FLOOR = 5;

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
                        dense[index].key.store(key, std::memory_order_relaxed);
                        return &dense[index];
                    }
                    current = expected;
                    continue;
                }
                if ((current >> 32) == tag) {
                    Entry* entry = &dense[(current & 0xFFFFFFFFULL) - 1];
                    // The tag is the wrong record about once every two billion
                    // probes, so the key still has to be checked.
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
        dst.delta.store(src.delta.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst.options.store(src.options.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // Drops every record that took fewer than VISIT_FLOOR visits in the pass just
    // ended, packs the survivors down, and rebuilds the slot array. Returns the
    // number dropped.
    //
    // Linear probing cannot delete in place: clearing one slot truncates the probe
    // chain for every key that probed past it, and those keys would then be
    // silently reinserted as duplicates. A full rebuild avoids that, and the caller
    // already holds every thread at a barrier.
    //
    // Survivors keep their relative order, so the hot early states stay at the
    // front. Indices below the new count are recycled by later inserts, which is
    // why the insert path can no longer assume a fresh record reads as zero.
    // keepVisits leaves a survivor's counter standing rather than rearming it for
    // the next pass. Only the pass that closes the run asks for it, so the counts
    // it cut on are the ones the exported file carries.
    uint64_t compact(bool keepVisits = false) {
        uint64_t n = used.load(std::memory_order_relaxed);
        uint64_t kept = 0;
        for (uint64_t i = 0; i < n; i++) {
            Entry& entry = dense[i];
            uint32_t visits = entry.visits.load(std::memory_order_relaxed);
            if (visits < VISIT_FLOOR) {
                continue;
            }
            if (kept != i) {
                copyRecord(dense[kept], entry);
            }
            dense[kept].visits.store(keepVisits ? visits : 0, std::memory_order_relaxed);
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

    // Pairs with the acquire loads above, so a thread that sees the slot also sees
    // a fully initialised record.
    void publish(uint64_t slotIndex, uint64_t word) {
        slots[slotIndex].store(word, std::memory_order_release);
    }
};
