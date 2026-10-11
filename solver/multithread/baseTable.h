#pragma once

#include "solver/multithread/config.h"

struct BaseEntry {
    uint64_t key;
    std::array<float, 4> weights;
};
static_assert(sizeof(BaseEntry) == 24, "BaseEntry must stay packed at 24 bytes");

struct BaseTable {
    std::vector<BaseEntry> slots;
    uint64_t mask = 0;
    uint64_t logSize = 0;
    uint64_t count = 0;

    void build(uint64_t n) {
        logSize = 4;
        while (double(n) > BASE_MAX_LOAD * double(1ULL << logSize)) {
            logSize++;
        }
        slots.assign(size_t(1ULL << logSize), BaseEntry{});
        mask = (1ULL << logSize) - 1;
        count = 0;
    }

    uint64_t indexOf(uint64_t key) const { return mix64(key) >> (64 - logSize); }

    void insert(uint64_t key, const std::array<float, 4>& weights) {
        if (key == 0) {
            throw std::runtime_error("the base file holds a zero key, which collides "
                "with the empty slot sentinel");
        }
        for (uint64_t i = indexOf(key); ; i = (i + 1) & mask) {
            if (slots[i].key == 0) {
                slots[i].key = key;
                slots[i].weights = weights;
                count++;
                return;
            }
            if (slots[i].key == key) {
                return;
            }
        }
    }

    const BaseEntry* find(uint64_t key) const {
        if (mask == 0) {
            return nullptr;
        }
        for (uint64_t i = indexOf(key); ; i = (i + 1) & mask) {
            const BaseEntry& entry = slots[i];
            if (entry.key == 0) {
                return nullptr;
            }
            if (entry.key == key) {
                return &entry;
            }
        }
    }

    uint64_t size() const { return count; }
    uint64_t slotCount() const { return slots.size(); }
};
