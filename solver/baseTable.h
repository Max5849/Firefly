#pragma once

#include "config.h"

// Option priors from a previous run. Read only after load and consulted only when
// a state is first inserted, so lookup latency is nearly free to spend and the
// layout is optimised for size instead.
//
// A hash map costs 24 bytes an entry: pair<uint64_t, uint8_t> pads to 16 because
// the key forces 8 byte alignment, and a separate bucket array adds 8 more. That
// is 7.8 GiB at a full scale base, which does not fit beside the strategy table.
// Structure of arrays removes the padding and open addressing removes the bucket
// array, leaving 9 bytes a slot.
struct BaseTable {
    std::vector<uint64_t> keys;
    // (fingerprint << 5) | options, zero meaning empty. The fingerprint is forced
    // into [1, 7] so an occupied slot is never zero even when its option mask is.
    // Rejecting on the fingerprint keeps a miss inside this 1 byte per slot array
    // rather than the 8 byte per slot keys, which matters because a miss walks the
    // whole cluster.
    std::vector<uint8_t> stamps;
    uint64_t mask = 0;
    uint64_t count = 0;

    static uint8_t stampOf(uint64_t hash, uint8_t options) {
        return uint8_t(((((hash >> 56) % 7) + 1) << 5) | (options & 0x1F));
    }

    // Sized to keep the load factor under 0.95. The index takes the low hash bits
    // and the fingerprint the top eight, so the two never overlap.
    void build(uint64_t n) {
        uint64_t size = 16;
        while (double(n) > 0.95 * double(size)) {
            size <<= 1;
        }
        keys.assign(size, 0);
        stamps.assign(size, 0);
        mask = size - 1;
        count = 0;
    }

    void insert(uint64_t key, uint8_t options) {
        const uint64_t hash = mix64(key);
        const uint8_t stamp = stampOf(hash, options);
        for (uint64_t i = hash & mask; ; i = (i + 1) & mask) {
            if (stamps[i] == 0) {
                keys[i] = key;
                stamps[i] = stamp;
                count++;
                return;
            }
            if (keys[i] == key) {
                return;
            }
        }
    }

    // The option mask, or -1 when the state is absent from the base.
    int find(uint64_t key) const {
        if (mask == 0) {
            return -1;
        }
        const uint64_t hash = mix64(key);
        const uint8_t want = uint8_t(((((hash >> 56) % 7) + 1) << 5));
        for (uint64_t i = hash & mask; ; i = (i + 1) & mask) {
            const uint8_t stamp = stamps[i];
            if (stamp == 0) {
                return -1;
            }
            if ((stamp & 0xE0) == want && keys[i] == key) {
                return stamp & 0x1F;
            }
        }
    }

    uint64_t size() const { return count; }
    uint64_t slots() const { return mask ? mask + 1 : 0; }
};
