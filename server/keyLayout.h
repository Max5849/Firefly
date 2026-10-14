#pragma once

#include "server/replay.h"

// The shared part of the state key, decoded field by field from the key's own bits.
//
// The node's key is actionState | metaState[combo] << 42. Bits 0..41 (the action
// state) and, postflop, bits 42..57 (the board field) are the same for every combo at
// a node; bits 58..63 are the combo's hand cluster and are left to the per-combo rows.
// Preflop bits 42..57 hold the combo's 169 class instead of a board, so there is no
// shared board field.
//
// Everything here reads the key, not the engine, so a field shows exactly what the key
// holds - bucketed, clamped, and zeroed where MEMORY_BASIC drops it. keyEquivalence
// checks each decoded value against the engine quantity it encodes.

struct KeyPart {
    std::string name;
    int value = 0;
    std::string meaning;
    // MEMORY_BASIC forces this part to 0.
    bool zeroed = false;
};

struct KeyField {
    std::string name;
    std::string label;
    int shift = 0;
    int width = 0;
    // Raw bits, in [0, 2^width).
    uint64_t value = 0;
    std::string meaning;
    // MEMORY_BASIC forces the whole field to 0.
    bool zeroed = false;
    // For a field that packs several values: its digits, most significant first.
    std::vector<KeyPart> parts;
    // Preflop the board field is per combo, so it has no shared value.
    bool shared = true;
};

struct KeyLayout {
    // actionState | boardField << 42, with the cluster bits clear. Preflop the board
    // field is 0.
    uint64_t sharedKey = 0;
    uint64_t actionState = 0;
    int heroSeat = 0;
    int villanSeat = 0;
    float spr = 0;
    float cpr = 0;
    std::vector<KeyField> fields;
};

inline constexpr float SPR_THRESHOLDS[8] = {0.289f, 0.5f, 0.866f, 1.5f, 2.6f, 4.5f, 7.8f, 13.5f};

inline std::string formatThreshold(float value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

// The ratio interval one SPR (0..8) or CPR (9..15) bucket covers. Under MEMORY_BASIC
// only every other threshold is cut, so a bucket spans two of the full intervals.
inline std::string sprCprInterval(int bucket, bool memoryBasic) {
    const bool isCpr = bucket >= 9;
    const int index = isCpr ? bucket - 9 : bucket;
    const int last = isCpr ? 6 : 8;
    const int step = memoryBasic ? 2 : 1;
    const std::string ratio = isCpr ? "CPR" : "SPR";
    const int low = index - step;
    if (index == 0) {
        return ratio + " < " + formatThreshold(SPR_THRESHOLDS[0]);
    }
    if (index >= last) {
        return ratio + " >= " + formatThreshold(SPR_THRESHOLDS[std::max(0, low)]);
    }
    return formatThreshold(SPR_THRESHOLDS[std::max(0, low)]) + " <= " + ratio + " < "
        + formatThreshold(SPR_THRESHOLDS[index]);
}

// raiseHistoryCompressed -> the per street raise counts it ranks. Built by playing
// every raise sequence through the increments Engine::play() adds.
inline const std::array<std::array<int, 4>, 256>& raiseHistoryTable() {
    static const std::array<std::array<int, 4>, 256> table = [] {
        std::array<std::array<int, 4>, 256> out;
        for (auto& entry : out) {
            entry = {-1, -1, -1, -1};
        }
        auto choose = [](int n, int k) {
            long value = 1;
            for (int i = 0; i < k; i++) {
                value = value * (n - i) / (i + 1);
            }
            return n < k ? 0 : int(value);
        };
        for (int r0 = 0; r0 <= 6; r0++) {
            for (int r1 = 0; r0 + r1 <= 6; r1++) {
                for (int r2 = 0; r0 + r1 + r2 <= 6; r2++) {
                    for (int r3 = 0; r0 + r1 + r2 + r3 <= 6; r3++) {
                        int value = 0;
                        for (int k = 0; k < r0; k++) value += choose(9 - k, 3);
                        for (int k = 0; k < r1; k++) value += choose(8 - r0 - k, 2);
                        for (int k = 0; k < r2; k++) value += 7 - r0 - r1 - k;
                        value += r3;
                        out[value] = {r0, r1, r2, r3};
                    }
                }
            }
        }
        return out;
    }();
    return table;
}

// The seats whose positionBasic is value, by name.
inline std::string positionBasicSeats(int value, int players) {
    std::vector<std::string> names = seatNames(players);
    std::string out;
    for (int seat = 0; seat < players; seat++) {
        int basic = seat > 2 ? 3 + (seat - 3) / 2 : seat;
        if (basic == value) {
            out += (out.empty() ? "" : "/") + names[seat];
        }
    }
    return out.empty() ? "no seat" : out;
}

inline std::string positionBucket(int value) {
    return value == 0 ? "0" : value == 1 ? "1-2" : "3+";
}

inline std::string countClamped(int value, const char* noun) {
    return (value >= 3 ? std::string("3+") : std::to_string(value)) + " " + noun;
}

inline uint64_t keyBits(uint64_t key, int shift, int width) {
    return (key >> shift) & ((1ULL << width) - 1);
}

// Decodes the acting seat's key at the node the replay reached. Must follow
// setActionState(config.memoryBasic).
template <int P>
KeyLayout decodeKey(const Replay<P>& replay) {
    const auto& engine = replay.engine;
    const StrategyConfig& config = replay.config;
    const bool memoryBasic = config.memoryBasic;
    const int round = engine.round;
    const auto& hero = engine.players[engine.action];

    KeyLayout out;
    out.actionState = memoryBasic ? engine.actionStateBasic : engine.actionState;
    out.heroSeat = engine.action;
    out.villanSeat = hero.villanPosition;
    out.spr = hero.spr;
    out.cpr = hero.cpr;
    uint64_t boardField = 0;
    if (round > 0) {
        // Any live combo carries the board half; every valid combo was classified.
        const auto& meta = config.metasBasic ? engine.metaStateBasic : engine.metaState;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (engine.commonRange[combo]) {
                boardField = meta[combo] & 0xFFFF;
                break;
            }
        }
    }
    out.sharedKey = out.actionState | boardField << 42;
    const uint64_t key = out.sharedKey;
    const std::vector<std::string> names = seatNames(P);

    auto add = [&](const char* name, const char* label, int shift, int width) -> KeyField& {
        KeyField field;
        field.name = name;
        field.label = label;
        field.shift = shift;
        field.width = width;
        field.value = keyBits(key, shift, width);
        out.fields.push_back(field);
        return out.fields.back();
    };

    {
        KeyField& field = add("headsBasic", "players in the hand", 0, 2);
        static const char* HEADS[4] = {"2 players", "3 players", "4-5 players", "6-7 players"};
        field.meaning = HEADS[field.value];
    }
    {
        KeyField& field = add("sprCpr", "stack or call to pot", 2, 4);
        field.meaning = (field.value >= 9 ? "calling is all in: " : "chips behind: ")
            + sprCprInterval(int(field.value), memoryBasic);
    }
    {
        KeyField& field = add("raiseHistory", "raises per street", 6, 8);
        const std::array<int, 4>& counts = raiseHistoryTable()[field.value];
        field.meaning = "colex rank of the raise counts";
        for (int street = 0; street < 4; street++) {
            KeyPart part;
            part.name = STREET_NAMES[street];
            part.value = counts[street];
            part.meaning = std::to_string(counts[street]) + " raise" + (counts[street] == 1 ? "" : "s");
            field.parts.push_back(part);
        }
    }
    {
        KeyField& field = add("story", "hero / villain story", 14, 11);
        uint64_t value = field.value;
        const int villanPos = int(value % 5);
        value /= 5;
        const int heroPos = int(value % 5);
        value /= 5;
        std::array<int, 4> digits;
        for (int street = 3; street >= 0; street--) {
            digits[street] = int(value % 3);
            value /= 3;
        }
        field.meaning = "one base 3 digit per street, then hero and villain position (base 5)";
        for (int street = 0; street < 4; street++) {
            KeyPart part;
            part.name = STREET_NAMES[street];
            part.value = digits[street];
            const int d = digits[street];
            if (street > round) {
                part.meaning = "not reached";
            } else if (street < round) {
                part.meaning = d == 0 ? "neither raised last"
                    : d == 1 ? "villain raised last" : "hero raised last";
            } else if (memoryBasic) {
                part.meaning = d == 0 ? "hero has not raised" : "hero has raised";
            } else {
                part.meaning = d == 0 ? "neither has raised" : d == 1 ? "villain has raised"
                    : "hero raised and villain re-raised";
            }
            field.parts.push_back(part);
        }
        const bool positionsZeroed = memoryBasic && round >= 1;
        KeyPart heroPart{"hero position", heroPos,
            positionsZeroed ? "dropped postflop" : positionBasicSeats(heroPos, P), positionsZeroed};
        KeyPart villanPart{"villain position", villanPos,
            positionsZeroed ? "dropped postflop" : positionBasicSeats(villanPos, P), positionsZeroed};
        field.parts.push_back(heroPart);
        field.parts.push_back(villanPart);
    }
    {
        KeyField& field = add("actionPositions", "action positions", 25, 5);
        const int value = int(field.value);
        field.meaning = "three base 3 digits: 0, 1-2, 3+ players";
        field.parts.push_back({"to act behind", value / 9, positionBucket(value / 9) + " still to act"});
        field.parts.push_back({"behind last raiser", value / 3 % 3,
            positionBucket(value / 3 % 3) + " left to act when the last raise was made"});
        field.parts.push_back({"behind raise before", value % 3,
            memoryBasic ? "dropped" : positionBucket(value % 3) + " left to act at the raise before",
            memoryBasic});
    }
    {
        KeyField& field = add("raiseSize", "last raise was big", 30, 1);
        field.meaning = field.value ? "last raise was 1.5x pot or all in" : "last raise was 0.5x pot, or none";
    }
    {
        KeyField& field = add("raiseStrength", "last raiser was facing", 31, 2);
        field.meaning = "the last raiser faced " + countClamped(int(field.value), "raises");
    }
    {
        KeyField& field = add("hiddenRaiseSize", "raise before was big", 33, 1);
        field.zeroed = memoryBasic;
        field.meaning = memoryBasic ? "dropped"
            : field.value ? "the raise before was 1.5x pot or all in" : "the raise before was 0.5x pot, or none";
    }
    {
        KeyField& field = add("hiddenRaiseStrength", "raise before was facing", 34, 2);
        field.zeroed = memoryBasic;
        field.meaning = memoryBasic ? "dropped"
            : "the raiser before faced " + countClamped(int(field.value), "raises");
    }
    {
        KeyField& field = add("raisesFaced", "raises hero faces", 36, 2);
        field.meaning = "hero faces " + countClamped(int(field.value), "raises") + " since acting";
    }
    {
        KeyField& field = add("relativePosition", "hero vs villain", 38, 1);
        field.meaning = field.value ? "hero acts after the villain postflop (in position)"
            : "hero acts at or before the villain postflop (out of position)";
    }
    {
        KeyField& field = add("heroCalls", "hero called twice", 39, 1);
        field.meaning = field.value ? "hero has called more than once this street"
            : "hero has called at most once this street";
    }
    {
        KeyField& field = add("tableCalls", "callers since the raise", 40, 2);
        field.zeroed = memoryBasic;
        static const char* CALLS[4] = {"0 callers", "1 caller", "2-3 callers", "4+ callers"};
        field.meaning = memoryBasic ? "dropped" : std::string(CALLS[field.value]) + " since the last raise";
    }
    {
        KeyField& field = add("boardField", "board class + 169", 42, 16);
        if (round == 0) {
            field.shared = false;
            field.meaning = "preflop: holds each combo's 169 class, so it is per combo";
        } else {
            field.meaning = std::string(config.metasBasic ? "basic" : "full") + " board class "
                + std::to_string(int(field.value) - 169);
        }
    }
    (void)names;
    return out;
}
