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
    uint32_t visits;
    uint16_t delta;
    double ratio;
    std::array<double, 4> weights;
    std::array<double, 5> probabilities;
    std::array<double, 5> sampled;
    float chips;
    float pot;

    bool updated;
    double finalRatio;
    double step;
    double target;
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
        out << "  seat " << node.action
            << "   combo " << node.combo << " (cards " << first << ", " << second << ")"
            << "   visits " << node.visits << "   delta " << node.delta
            << "   invTemp " << INV_TEMP
            << "   chips " << node.chips << "   pot " << node.pot << "\n";
        out << std::setprecision(6);
        out << "  ratio     at decision " << node.ratio
            << "   at end " << node.finalRatio << "\n";
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
            out << "  update    none, the phase multiplier or the final ratio is 0\n";
        } else {
            out << "  return    " << node.target << " of the pot\n";
            out << "  update    " << optionName(node.option) << " "
                << node.before << " -> " << node.after
                << "  (change " << (node.after - node.before)
                << ", step " << node.step << ")\n";
        }
    }
}
