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

// One decision that reached TRACE_KEY. The acting seat holds one hand, so the node is
// looked up once and takes at most one EMA step.
struct TraceNode {
    uint64_t key;
    int decision;
    int combo;
    int action;
    int option;
    uint8_t legal;
    // Every stored option's statistics (call, the two raises, all in), read at the
    // decision, before this hand's update. Fold has none: its return is the exact 0.
    std::array<double, 4> visits;
    std::array<double, 4> means;
    std::array<double, 4> variances;
    double ratio;
    double certainty;
    std::array<double, 4> weights;
    std::array<double, 5> probabilities;
    std::array<double, 5> sampled;
    float chips;
    float pot;

    bool updated;
    double finalRatio;
    double stepRatio;
    double onward;
    double inverseVariance;
    double step;
    double target;
    // The return times the hand certainty: what the weight stepped toward.
    double weightTarget;
    double before;
    double after;
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
        double nodeVisits = 0;
        for (double visits : node.visits) {
            nodeVisits += visits;
        }
        out << "  seat " << node.action
            << "   combo " << node.combo << " (cards " << first << ", " << second << ")"
            << "   node visits " << std::defaultfloat << nodeVisits << std::fixed
            << "   invTemp " << INV_TEMP
            << "   chips " << node.chips << "   pot " << node.pot << "\n";
        out << std::setprecision(6);
        out << "  ratio     at decision " << node.ratio
            << "   at end " << node.finalRatio;
        if (node.updated) {
            out << "   without its own " << node.stepRatio;
        }
        out << "\n";
        out << "  certainty  own " << node.certainty;
        if (node.updated) {
            out << "   hand " << node.onward << "   1 / variance " << node.inverseVariance;
        }
        out << "\n";
        out << "  weights ";
        for (int j = 0; j < 5; j++) {
            out << "  " << optionName(j) << " ";
            if (!(node.legal & (1u << j))) {
                out << "-";
            } else if (j == 0) {
                out << "0.000000 (exact)";
            } else {
                out << node.weights[j - 1];
            }
        }
        out << "\n";
        // Visits are importance weighted in 1/65536 units, so they print in general
        // notation: a count of 3e-05 must not read as 0.000.
        auto statsRow = [&](const char* label, const std::array<double, 4>& values, bool general) {
            out << label;
            for (int j = 0; j < 5; j++) {
                out << "  " << optionName(j) << " ";
                if (!(node.legal & (1u << j))) {
                    out << "-";
                } else if (j == 0) {
                    out << "none";
                } else if (general) {
                    out << std::defaultfloat << values[j - 1] << std::fixed;
                } else {
                    out << values[j - 1];
                }
            }
            out << "\n";
        };
        statsRow("  visits  ", node.visits, true);
        statsRow("  mean    ", node.means, false);
        statsRow("  variance", node.variances, false);
        out << std::setprecision(4);
        out << "  policy  ";
        for (int j = 0; j < 5; j++) {
            out << "  " << optionName(j) << " " << 100.0 * node.probabilities[j] << "%";
        }
        out << "\n";
        out << "  sampled ";
        for (int j = 0; j < 5; j++) {
            out << "  " << optionName(j) << " " << 100.0 * node.sampled[j] << "%";
        }
        out << "\n";
        out << "  chosen    " << optionName(node.option) << "\n";
        out << std::setprecision(6);
        if (node.option == 0) {
            out << "  update    none, fold has no stored weight\n";
        } else if (!node.updated) {
            out << "  update    none: warmup, a 0 ratio, or its visits rounded to 0\n";
        } else {
            out << "  return    " << node.target << " pots   x hand certainty "
                << node.onward << " = target " << node.weightTarget << " pots\n";
            out << "  update    " << optionName(node.option) << " "
                << node.before << " -> " << node.after
                << "  (change " << (node.after - node.before)
                << ", step " << node.step << ")\n";
        }
    }
}
