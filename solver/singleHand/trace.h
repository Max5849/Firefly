#pragma once

#include "solver/singleHand/config.h"
#include "solver/singleHand/engine.h"

static inline const char* optionName(int option) {
    switch (option) {
        case 0:  return "fold";
        case 1:  return "call/check";
        case 2:  return "raise 0.5x pot";
        case 3:  return "raise 1.5x pot";
        default: return "all in";
    }
}

// One traverser decision that reached TRACE_KEY and took its update. Every legal option
// was played out, so each stored one takes its own step.
struct TraceNode {
    uint64_t key;
    int combo;
    int seat;
    uint8_t legal;
    bool seeded;
    // The node's visits after this one, and the share this visit took of them.
    double visits;
    double share;
    // Every stored option's variance (call, the two raises, all in), read before the
    // update. Fold has none: its return is the exact 0.
    std::array<double, 4> variances;
    // The traverser's own probability of the branch.
    double reach;
    std::array<double, 5> probabilities;
    float chips;
    float pot;
    // The decision's return under its policy, in pots.
    double value;

    bool updated;
    // Whether the option stepped as a running mean rather than an EMA.
    std::array<bool, 4> running;
    std::array<double, 4> step;
    std::array<double, 4> target;
    std::array<double, 4> before;
    std::array<double, 4> after;
    std::array<double, 4> variancesAfter;
};

inline void formatTrace(const std::vector<TraceNode>& tracePath, std::ostringstream& out) {
    out << std::fixed;
    for (const TraceNode& node : tracePath) {
        int first = 0;
        while (holePairIndex(first + 1, first + 2) <= node.combo) {
            first++;
        }
        const int second = node.combo + 1 - first * (101 - first) / 2;
        out << "===== trace key 0x" << std::hex << std::setw(16) << std::setfill('0')
            << node.key << std::dec << std::setfill(' ') << " =====\n";
        out << std::setprecision(3);
        out << "  seat " << node.seat
            << "   combo " << node.combo << " (cards " << first << ", " << second << ")"
            // Visits are reach weighted in 1/65536 units, so they print in general
            // notation: a count of 3e-05 must not read as 0.000.
            << "   node visits " << std::defaultfloat << node.visits << std::fixed
            << (node.seeded ? "   seeded" : "")
            << "   invTemp " << INV_TEMP
            << "   chips " << node.chips << "   pot " << node.pot << "\n";
        out << std::setprecision(6);
        out << "  reach     " << node.reach << "   share " << std::defaultfloat << node.share
            << std::fixed << "   value " << node.value << " pots\n";
        out << std::setprecision(4);
        out << "  policy  ";
        for (int j = 0; j < 5; j++) {
            out << "  " << optionName(j) << " " << 100.0 * node.probabilities[j] << "%";
        }
        out << "\n";
        out << std::setprecision(6);
        for (int j = 1; j < 5; j++) {
            if (!(node.legal & (1u << j))) {
                continue;
            }
            const int o = j - 1;
            out << "  " << optionName(j) << ": " << (node.running[o] ? "mean" : "ema ")
                << "   return " << node.target[o] << " pots   weight "
                << node.before[o] << " -> " << node.after[o]
                << "  (step " << std::defaultfloat << node.step[o] << std::fixed
                << ")   variance " << node.variances[o] << " -> " << node.variancesAfter[o] << "\n";
        }
    }
}
