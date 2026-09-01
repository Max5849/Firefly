#pragma once

#include "server/replay.h"
#include "server/strategyStore.h"

// The sampler's exponent clamp, copied from explorer.cpp, which cannot be
// included because it carries main(). It never binds in practice - the worst
// exponent measured in a real run is 47 - and only stops a runaway weight from
// turning the option total into inf or zero.
inline constexpr double MAX_EXP_ARG = 700.0;

// The inverse temperature the run finished at. Frequencies at this value are the
// policy the solver was actually sampling from at the end of training.
inline const double TERMINAL_INV_TEMP = std::log(65536.0);

inline constexpr int COMBO_COUNT = 1326;

// The 1326 combo addressing used throughout handAnalysis: for cards i < j,
// index = i * (101 - i) / 2 + j - 1.
inline int holePairIndex(int i, int j) {
    return i * (101 - i) / 2 + j - 1;
}

struct ComboRow {
    // Clashes with a board card, so this hand cannot be held.
    bool valid = false;
    // The key was present in the strategy table.
    bool found = false;
    // The (boardClass, handClass) pair was missing from handMetas. Should never
    // happen for a legal board; reported rather than thrown.
    bool metaFound = true;
    uint64_t key = 0;
    std::array<int, 2> cards = {0, 0};
    std::array<double, 4> weights = {};
    std::array<double, 5> frequencies = {};
    uint32_t visits = 0;
};

struct SweepResult {
    std::array<ComboRow, COMBO_COUNT> rows;
    std::array<bool, 5> legal = {};
    int found = 0;
    int missing = 0;
    int invalid = 0;
    int metaMissing = 0;
};

// Fills in one row's frequencies from its weights.
//
//   w[0] = 0, exactly: folding's pot normalised return is the analytic zero every
//          other weight is measured against, not a placeholder.
//   P(j) proportional to exp(clamp(invTemp * w[j], +-700)), over legal j only.
//
// Illegal options are left out of the sum rather than given a zero weight. Their
// stored slot holds whatever was in memory, and including it would distort the
// distribution over the options that are real.
inline void computeFrequencies(const std::array<double, 4>& weights,
        const std::array<bool, 5>& legal, double invTemp,
        std::array<double, 5>& frequencies) {
    std::array<double, 5> draws = {};
    double sum = 0;
    if (legal[0]) {
        draws[0] = 1.0;
    }
    sum += draws[0];
    for (int j = 1; j < 5; j++) {
        if (legal[j]) {
            draws[j] = std::exp(std::clamp(invTemp * weights[j - 1],
                -MAX_EXP_ARG, MAX_EXP_ARG));
        }
        sum += draws[j];
    }
    for (int j = 0; j < 5; j++) {
        frequencies[j] = sum > 0 ? draws[j] / sum : 0.0;
    }
}

// Puts one hole card combination in front of the acting player and returns the
// state key for it.
//
// Hole cards enter the 64 bit key only through its 20 bit board/meta field, and
// getState() never reads a villain's hand. So a node is replayed once and each
// combination costs one classifyHand, two map lookups and one getState - rather
// than a replay apiece.
//
// Returns false only when the (boardClass, handClass) pair is missing from
// handMetas, which should not happen for a legal board.
inline bool comboKey(Engine& engine, int first, int second, uint64_t& key) {
    Engine::Player& player = engine.players[engine.action];
    if (engine.round == 0) {
        // Preflop the key carries the 169 combo index directly, exactly as
        // deal() computes it. 168 is the largest, so it survives Player's uint8_t
        // fields intact.
        int rank0 = first / 4;
        int rank1 = second / 4;
        int index = rank0 * (25 - rank0) / 2 + rank1
            + (90 - rank0) * (first % 4 == second % 4);
        player.handMeta = uint8_t(index);
        player.handMetaBasic = uint8_t(index);
    } else {
        std::array<int, 2> holeCards = {first, second};
        uint32_t handClass = engine.handClusterer.classifyHand(
            engine.boardCards, holeCards, engine.round);
        // The meta key uses the current street's board class, as newRound() does,
        // even under METAS_BASIC where it is only the key's board field that
        // switches to the flop class.
        uint64_t metaKey = uint64_t(engine.boardClasses[engine.round])
            | (uint64_t(handClass) << 32);
        // find() rather than at(): a throw inside a request handler, for an input
        // the caller controls, is the wrong failure mode.
        auto basicAt = Engine::handMetasBasic.find(metaKey);
        auto at = Engine::handMetas.find(metaKey);
        if (basicAt == Engine::handMetasBasic.end() || at == Engine::handMetas.end()) {
            return false;
        }
        player.handMeta = at->second;
        player.handMetaBasic = basicAt->second;
    }
    player.holeCards = {first, second};
    key = engine.getState(METAS_BASIC, MEMORY_BASIC);
    return true;
}

// Reads the weights for every hole card combination at the node the replay has
// reached.
inline SweepResult sweep(Engine& engine, const std::vector<int>& board,
        const StrategyStore& store, double invTemp) {
    SweepResult result;

    // getOptions() reads spr and cpr, which getState() assigns, and neither
    // depends on hole cards - so legality is computed once for the whole sweep.
    engine.getState(METAS_BASIC, MEMORY_BASIC);
    result.legal = engine.getOptions();

    std::array<bool, 52> onBoard = {};
    for (int card : board) {
        onBoard[card] = true;
    }

    StrategyRecord record;
    for (int first = 0; first < 52; first++) {
        for (int second = first + 1; second < 52; second++) {
            ComboRow& row = result.rows[holePairIndex(first, second)];
            row.cards = {first, second};
            if (onBoard[first] || onBoard[second]) {
                result.invalid++;
                continue;
            }
            row.valid = true;
            if (!comboKey(engine, first, second, row.key)) {
                row.metaFound = false;
                result.metaMissing++;
                continue;
            }
            if (store.lookup(row.key, record)) {
                row.found = true;
                row.weights = record.weights;
                row.visits = record.visits;
                computeFrequencies(row.weights, result.legal, invTemp, row.frequencies);
                result.found++;
            } else {
                // The table only holds states self play actually reached. A
                // legitimate node can be absent, and there is nothing nearby to
                // fall back on, so the row says so and the caller decides.
                result.missing++;
            }
        }
    }
    return result;
}
