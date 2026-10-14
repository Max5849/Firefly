#pragma once

#include "server/replay.h"
#include "server/strategyStore.h"

// The strategy at the node a replay reached, for every one of the 1326 combos the
// acting seat could hold, and its range.
//
// Combos with the same hand cluster share a key and therefore a record, so a node is
// one lookup per cluster, not per combo.

struct SpotRow {
    // False when the combo uses a revealed board card.
    bool valid = false;
    std::array<int, 2> cards = {0, 0};
    // The acting seat's reach: the product of its own INV_TEMP policy along the line,
    // after RANGE_FLOOR. 0 for a combo the line has ruled out.
    float reach = 0;
    // The hand part of the key: the 169 class preflop, the hand cluster postflop.
    int cluster = 0;
    uint64_t key = 0;
    bool found = false;
    // Option 1..4 values from the record, pot normalised, relative to folding.
    std::array<double, 4> weights = {};
    // The INV_TEMP policy over the legal options.
    std::array<double, 5> frequencies = {};
    // Each option's value in chips relative to folding (weight * pot after calling),
    // and their mean under the policy.
    std::array<double, 5> evOptions = {};
    double ev = 0;
    // The record's visits (cooldown block, reach weighted), shared by its cluster, and
    // how many valid combos share it.
    uint32_t visits = 0;
    int clusterCombos = 0;
};

struct SpotResult {
    std::array<SpotRow, COMBO_COUNT> rows;
    std::array<bool, 5> legal = {};
    // The weight denominator: the pot once the actor has called.
    double potAfterCall = 0;
    // The sum of visits over the distinct records the acting range reaches.
    uint64_t totalVisits = 0;
    int records = 0;
    int found = 0;
    int missing = 0;
    int invalid = 0;
    // Combos with reach > 0, their summed reach, and the share of it whose node has a
    // record.
    int rangeCombos = 0;
    double rangeWeight = 0;
    double rangeFoundWeight = 0;
    // The range's action mix and mean EV, reach weighted over the combos with a record.
    std::array<double, 5> rangeMix = {};
    double rangeEv = 0;
    // Seats, live or folded, whose range the line has emptied. The trainer drops such
    // a hand at the next street.
    std::vector<int> emptySeats;
};

template <int P>
SpotResult computeSpot(const Replay<P>& replay, const StrategyStore& store,
        const std::array<bool, 5>& legal) {
    const auto& engine = replay.engine;
    const StrategyConfig& config = replay.config;
    SpotResult out;

    const auto& actor = engine.players[engine.action];
    // getOptions() at this node, from the replay. Not stored in the strategy file.
    out.legal = legal;
    out.potAfterCall = double(engine.pot + (engine.bet - actor.bet));

    const uint64_t actionState = config.memoryBasic ? engine.actionStateBasic : engine.actionState;
    const auto& meta = config.metasBasic ? engine.metaStateBasic : engine.metaState;

    struct Group {
        bool looked = false;
        bool found = false;
        StrategyRecord record;
        int combos = 0;
        bool reached = false;
    };
    std::array<Group, GROUP_COUNT> groups;
    std::array<uint8_t, COMBO_COUNT> groupOf;

    for (int first = 0; first < 52; first++) {
        for (int second = first + 1; second < 52; second++) {
            const int combo = holePairIndex(first, second);
            SpotRow& row = out.rows[combo];
            row.cards = {first, second};
            if (!engine.commonRange[combo]) {
                out.invalid++;
                continue;
            }
            row.valid = true;
            row.reach = actor.range[combo];
            const uint64_t comboMeta = meta[combo];
            row.cluster = int(engine.round == 0 ? comboMeta : comboMeta >> 16);
            row.key = actionState | comboMeta << 42;
            const uint8_t g = uint8_t(row.cluster);
            groupOf[combo] = g;
            Group& group = groups[g];
            if (!group.looked) {
                group.looked = true;
                group.found = store.lookup(row.key, group.record);
            }
            group.combos++;
            group.reached |= row.reach > 0.0f;
            row.found = group.found;
            if (!row.found) {
                out.missing++;
                continue;
            }
            out.found++;
            std::array<double, 5> values = {};
            for (int j = 1; j < 5; j++) {
                row.weights[j - 1] = double(group.record.weights[j - 1]);
                values[j] = row.weights[j - 1];
            }
            row.frequencies = policyFromWeights(group.record.weights, out.legal);
            row.visits = group.record.visits;
            row.ev = 0;
            for (int j = 0; j < 5; j++) {
                row.evOptions[j] = out.legal[j] ? values[j] * out.potAfterCall : 0.0;
                row.ev += row.frequencies[j] * row.evOptions[j];
            }
        }
    }

    for (int combo = 0; combo < COMBO_COUNT; combo++) {
        SpotRow& row = out.rows[combo];
        if (!row.valid) {
            continue;
        }
        row.clusterCombos = groups[groupOf[combo]].combos;
        if (row.reach > 0.0f) {
            out.rangeCombos++;
            out.rangeWeight += row.reach;
            if (row.found) {
                out.rangeFoundWeight += row.reach;
                out.rangeEv += row.reach * row.ev;
                for (int j = 0; j < 5; j++) {
                    out.rangeMix[j] += row.reach * row.frequencies[j];
                }
            }
        }
    }
    if (out.rangeFoundWeight > 0) {
        out.rangeEv /= out.rangeFoundWeight;
        for (double& share : out.rangeMix) {
            share /= out.rangeFoundWeight;
        }
    }
    for (const Group& group : groups) {
        if (group.looked && group.found) {
            out.records++;
            if (group.reached) {
                out.totalVisits += group.record.visits;
            }
        }
    }
    for (int seat = 0; seat < P; seat++) {
        bool live = false;
        for (int combo = 0; combo < COMBO_COUNT && !live; combo++) {
            live = engine.commonRange[combo] && engine.players[seat].range[combo] > 0.0f;
        }
        if (!live) {
            out.emptySeats.push_back(seat);
        }
    }
    return out;
}
