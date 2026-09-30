#pragma once

#include "server/replay.h"
#include "server/strategyStore.h"

#include <cmath>

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
    // The hand meta the key branches on: the 169 combo index preflop, the 5 bit
    // cluster postflop. This is handMetaBasic, since METAS_BASIC selects it.
    uint8_t handMeta = 0;
    std::array<double, 4> weights = {};
    std::array<double, 5> frequencies = {};
    // The record's raw count, shared by every combination with this key.
    uint32_t nodeVisits = 0;
    // How many valid combinations in this sweep produce this same key.
    int metaCombos = 0;
    // nodeVisits / metaCombos: the estimated visits for this exact holding rather
    // than for its whole cluster.
    double visits = 0.0;
};

// Which per-combination fields the caller asked for. Anything off is not computed,
// not merely not serialised - with every field off the sweep does not run at all.
struct SweepFields {
    bool weights = false;
    bool frequencies = false;
    bool visits = false;
    bool key = false;
    bool handMeta = false;
    bool counts = false;

    // The row's presence in the strategy table is only needed for the fields that
    // come out of the record.
    bool needsStore() const { return weights || frequencies || visits || counts; }
    bool any() const { return needsStore() || key || handMeta; }
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
// reached. Work is gated on fields: nothing the caller did not ask for is
// computed, so a request wanting only handMeta never touches the strategy file.
inline SweepResult sweep(Engine& engine, const std::vector<int>& board,
        const StrategyStore& store, double invTemp, const SweepFields& fields) {
    SweepResult result;

    // getOptions() reads spr and cpr, which getState() assigns, and neither
    // depends on hole cards - so legality is computed once for the whole sweep.
    engine.getState(METAS_BASIC, MEMORY_BASIC);
    result.legal = engine.getOptions();

    std::array<bool, 52> onBoard = {};
    for (int card : board) {
        onBoard[card] = true;
    }

    const bool needsStore = fields.needsStore();
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
            row.handMeta = engine.players[engine.action].handMetaBasic;
            if (!needsStore) {
                continue;
            }
            if (store.lookup(row.key, record)) {
                row.found = true;
                // Stored as float, widened here so the softmax below and the JSON
                // it feeds stay in double.
                for (int j = 0; j < 4; j++) {
                    row.weights[j] = double(record.weights[j]);
                }
                row.nodeVisits = record.visits;
                if (fields.frequencies) {
                    computeFrequencies(row.weights, result.legal, invTemp,
                        row.frequencies);
                }
                result.found++;
            } else {
                // The table only holds states self play actually reached. A
                // legitimate node can be absent, and there is nothing nearby to
                // fall back on, so the row says so and the caller decides.
                result.missing++;
            }
        }
    }

    // visits is stored against the key, and a key is shared by every combination
    // with the same hand meta - so the raw count overstates one holding by the
    // size of its cluster. Divide it by how many valid combinations reach this
    // same key. Every row in a group shares a key and therefore a record, so found
    // is uniform within a group and misses need no special case.
    if (fields.visits) {
        ankerl::unordered_dense::map<uint64_t, int> groupSizes;
        groupSizes.reserve(COMBO_COUNT);
        for (const ComboRow& row : result.rows) {
            if (row.valid && row.metaFound) {
                groupSizes[row.key]++;
            }
        }
        for (ComboRow& row : result.rows) {
            if (row.valid && row.metaFound) {
                row.metaCombos = groupSizes[row.key];
                row.visits = double(row.nodeVisits) / double(row.metaCombos);
            }
        }
    }
    return result;
}

// The part of the key every one of the 1326 hands shares, decomposed into named
// components.
//
// Values are reported as the key encodes them - bucketed, clamped, and zeroed
// where MEMORY_BASIC drops a field - not as the underlying quantities. The one
// exception is raiseHistory, which replaces raiseHistoryCompressed: the four
// per-street counts are what a reader wants, and the compressed byte is a colex
// rank of exactly those counts.
//
// This is the only code in server/ that reproduces getState()'s encoding instead
// of calling it, so it can drift silently. keyEquivalence masks every field out of
// the real key and compares, at every node it visits.
struct KeyComponents {
    int roundHeadsBasic = 0;
    int headsBasic = 0;
    int actionPositionBucket = 0;
    int sprCprBucket = 0;
    // "spr" when the player has chips behind after calling, "cpr" when calling is
    // already all-in and the price carries the information instead.
    const char* sprCprKind = "spr";
    // Preflop the 15 bit board field carries the 169 combo index, which is hand
    // dependent - so there is no shared board field to report and this is false.
    bool hasBoardField = false;
    int boardField = 0;
    int boardClass = 0;
    std::array<int, 4> raiseHistory = {};
    std::array<int, 4> playerLastActions = {};
    std::array<int, 4> villanLastActions = {};
    int playerPositionBasic = 0;
    int villanPositionBasic = 0;
    // Not a key field: it selects which seat the two villan fields describe, and
    // they cannot be read without it.
    int villanPosition = 0;
    int raiseSizeOverZero = 0;
    int raiseStrength = 0;
    int hiddenRaiseSizeOverZero = 0;
    int hiddenRaiseStrength = 0;
    int raisesFaced = 0;
    int relativePosition = 0;
    bool playerCallsOverOne = false;
    int calls = 0;
};

// The SPR / CPR bucket, bits 6-9. Duplicated from getState() because it is the
// only component whose encoding is more than a mask or a clamp; the thresholds are
// geometric with ratio sqrt(3), so each bucket is half an octave of pot odds.
inline int sprCprBucket(float spr, float cpr, bool& isSpr) {
    static constexpr float THRESHOLDS[8] =
        {0.289f, 0.5f, 0.866f, 1.5f, 2.6f, 4.5f, 7.8f, 13.5f};
    if (spr < 0.01f) {
        // Calling is effectively all-in, so stack depth carries no information and
        // the price does. Buckets 9..15 against the first six thresholds.
        isSpr = false;
        for (int i = 0; i < 6; i++) {
            if (cpr < THRESHOLDS[i]) {
                return 9 + i;
            }
        }
        return 15;
    }
    isSpr = true;
    for (int i = 0; i < 8; i++) {
        if (spr < THRESHOLDS[i]) {
            return i;
        }
    }
    return 8;
}

// Must be called after a getState(), which assigns player.spr, player.cpr and
// player.actionPosition as a side effect.
inline KeyComponents keyComponents(const Engine& engine) {
    const Engine::Player& player = engine.players[engine.action];
    const Engine::Player& villan = engine.players[player.villanPosition];
    // MEMORY_BASIC zeroes the two positionBasic fields from the turn onward.
    // Report the zeros that are genuinely in the key rather than undoing them.
    //
    // It does NOT touch the hiddenRaise fields: the if (!memoryBasic) that used to
    // wrap them was removed in commit 1b24d24, so they are always encoded. The
    // solver skill still documents the old behaviour.
    const bool keepPositions = engine.round < 2 || !MEMORY_BASIC;

    KeyComponents out;
    out.roundHeadsBasic = engine.roundHeadsBasic & 0x3;
    out.headsBasic = engine.headsBasic & 0x3;
    out.actionPositionBucket = player.actionPosition & 0x3;

    bool isSpr = true;
    out.sprCprBucket = sprCprBucket(player.spr, player.cpr, isSpr);
    out.sprCprKind = isSpr ? "spr" : "cpr";

    if (engine.round != 0) {
        out.hasBoardField = true;
        // METAS_BASIC keys the board field on the flop class plus the street, so
        // turn and river cards create no new board branches.
        out.boardClass = METAS_BASIC ? engine.boardClasses[1] : engine.boardClasses[engine.round];
        out.boardField = (out.boardClass + 169 + (METAS_BASIC ? engine.round : 0)) & 0x7FFF;
    }

    for (int i = 0; i < 4; i++) {
        out.raiseHistory[i] = engine.raiseHistory[i];
        out.playerLastActions[i] = player.lastActions[i] & 0x1;
        out.villanLastActions[i] = villan.lastActions[i] & 0x1;
    }
    out.playerPositionBasic = keepPositions ? (player.positionBasic & 0x7) : 0;
    out.villanPositionBasic = keepPositions ? (villan.positionBasic & 0x7) : 0;
    out.villanPosition = player.villanPosition;

    out.raiseSizeOverZero = engine.raiseSize > 0 ? 1 : 0;
    out.raiseStrength = std::min(engine.raiseStrength, uint8_t(3)) & 0x3;
    out.hiddenRaiseSizeOverZero = engine.hiddenRaiseSize > 0 ? 1 : 0;
    out.hiddenRaiseStrength = std::min(engine.hiddenRaiseStrength, uint8_t(3)) & 0x3;
    out.raisesFaced = std::min(player.raisesFaced, uint8_t(3)) & 0x3;
    out.relativePosition = player.relativePosition & 0x1;
    out.playerCallsOverOne = player.calls > 1;
    out.calls = engine.calls < 2 ? engine.calls : (engine.calls < 4 ? 2 : 3);
    return out;
}
