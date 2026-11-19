#pragma once

#include "server/replay.h"

// The shared part of the state key, decoded field by field from the key's own bits.
//
// The node's key is actionState | metaState[combo] << ACTION_STATE_BITS (43). Bits 0..42
// (the action state) and, postflop, bits 43..57 (the board field) are the same for every
// combo at a node; bits 58..63 are the combo's hand cluster and are left to the
// per-combo rows. Preflop bits 43..57 hold the combo's 169 class instead of a board, so
// there is no shared board field.
//
// Everything here reads the key, not the engine, so a field shows exactly what the key
// holds - bucketed, clamped, and zeroed where Engine::setActionState leaves it 0.
// keyEquivalence checks each decoded value against the engine quantity it encodes.
//
// Every memory stage currently builds the same action state, so the layout does not
// depend on the stage.

struct KeyPart {
    std::string name;
    int value = 0;
    std::string meaning;
    // The key holds this part at 0 here, whatever the hand did.
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
    // The key holds the whole field at 0 here, whatever the hand did.
    bool zeroed = false;
    // For a field that packs several values: its digits, most significant first.
    std::vector<KeyPart> parts;
    // Preflop the board field is per combo, so it has no shared value.
    bool shared = true;
};

struct KeyLayout {
    // actionState | boardField << ACTION_STATE_BITS, with the cluster bits clear.
    // Preflop the board field is 0.
    uint64_t sharedKey = 0;
    uint64_t actionState = 0;
    int heroSeat = 0;
    int villanSeat = 0;
    float spr = 0;
    float cpr = 0;
    std::vector<KeyField> fields;
};

// The bits every combo at a node shares: the action state and the board field.
inline constexpr int SHARED_KEY_BITS = ACTION_STATE_BITS + BOARD_CLASS_BITS;

// setActionState's cuts: SPR buckets 0..5, and with no chips behind, CPR buckets 6..9.
inline constexpr float SPR_THRESHOLDS[5] = {0.5f, 1.5f, 3.5f, 7.5f, 15.5f};
inline constexpr float CPR_THRESHOLDS[3] = {0.5f, 1.5f, 3.5f};

inline std::string formatThreshold(float value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string ratioInterval(const char* ratio, const float* cuts, int count, int index) {
    if (index == 0) {
        return std::string(ratio) + " < " + formatThreshold(cuts[0]);
    }
    if (index >= count) {
        return std::string(ratio) + " >= " + formatThreshold(cuts[count - 1]);
    }
    return formatThreshold(cuts[index - 1]) + " <= " + ratio + " < " + formatThreshold(cuts[index]);
}

// The ratio interval one bucket covers: SPR for 0..5, CPR for 6..9.
inline std::string sprCprInterval(int bucket) {
    if (bucket >= 6) {
        return "calling is all in: " + ratioInterval("CPR", CPR_THRESHOLDS, 3, bucket - 6);
    }
    return "chips behind: " + ratioInterval("SPR", SPR_THRESHOLDS, 5, bucket);
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

// bucket(x) = (x > 0) + (x > 2), for seats left to act.
inline std::string positionBucket(int value) {
    return value == 0 ? "0" : value == 1 ? "1-2" : "3+";
}

inline std::string countClamped(int value, const char* noun) {
    return (value >= 2 ? std::string("2+") : std::to_string(value)) + " " + noun;
}

inline std::string headsBucket(int value) {
    static const char* HEADS[4] = {"2 players", "3 players", "4-5 players", "6-7 players"};
    return HEADS[value & 3];
}

// preflopHeadsBasic keeps only the low bit of the heads bucket.
inline std::string headsBucketLowBit(int value) {
    return value & 1 ? "3 or 6-7 players" : "2 or 4-5 players";
}

inline uint64_t keyBits(uint64_t key, int shift, int width) {
    return (key >> shift) & ((1ULL << width) - 1);
}

// Decodes the acting seat's key at the node the replay reached. Must follow
// setActionState(config.memoryStage).
template <int P>
KeyLayout decodeKey(const Replay<P>& replay) {
    const auto& engine = replay.engine;
    const StrategyConfig& config = replay.config;
    const int round = engine.round;
    const auto& hero = engine.players[engine.action];

    KeyLayout out;
    out.actionState = engine.getActionState(config.memoryStage);
    out.heroSeat = engine.action;
    out.villanSeat = hero.villanPosition;
    out.spr = hero.spr;
    out.cpr = hero.cpr;
    uint64_t boardField = 0;
    if (round > 0) {
        // Any combo off the board carries the board half; every one was classified.
        const auto& meta = engine.getMetaState(config.metasStage);
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (engine.commonRange[combo]) {
                boardField = meta[combo] & BOARD_CLASS_MASK;
                break;
            }
        }
    }
    out.sharedKey = out.actionState | boardField << ACTION_STATE_BITS;
    const uint64_t key = out.sharedKey;

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
        field.meaning = headsBucket(int(field.value));
    }
    {
        KeyField& field = add("preflopHeadsBasic", "players who saw the flop (low bit)", 2, 1);
        field.meaning = (round == 0 ? "preflop, so the players in the hand: "
            : "players left when preflop ended: ") + headsBucketLowBit(int(field.value));
    }
    {
        KeyField& field = add("sprCpr", "stack or call to pot", 3, 4);
        field.meaning = sprCprInterval(int(field.value));
    }
    std::array<int, 4> raiseCounts;
    {
        KeyField& field = add("raiseHistory", "raises per street", 7, 8);
        raiseCounts = raiseHistoryTable()[field.value];
        field.meaning = "colex rank of the raise counts";
        for (int street = 0; street < 4; street++) {
            KeyPart part;
            part.name = STREET_NAMES[street];
            part.value = raiseCounts[street];
            part.meaning = std::to_string(raiseCounts[street]) + " raise"
                + (raiseCounts[street] == 1 ? "" : "s");
            field.parts.push_back(part);
        }
    }
    {
        KeyField& field = add("raiseSizes", "last raise was big, per street", 15, 4);
        field.meaning = "one bit per street: its last raise was 1.5x pot or all in";
        for (int street = 0; street < 4; street++) {
            KeyPart part;
            part.name = STREET_NAMES[street];
            part.value = int(field.value >> street & 1);
            part.meaning = raiseCounts[street] <= 0 ? "no raise"
                : part.value ? "1.5x pot or all in" : "0.5x pot";
            field.parts.push_back(part);
        }
    }
    {
        KeyField& field = add("story", "hero / villain story", 19, 11);
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
                part.zeroed = true;
            } else if (street < round) {
                part.meaning = d == 0 ? "neither raised last"
                    : d == 1 ? "villain raised last" : "hero raised last";
            } else {
                part.meaning = d == 0 ? "neither has raised"
                    : d == 1 ? "one of hero and villain raised last" : "both raised last";
            }
            field.parts.push_back(part);
        }
        field.parts.push_back({"hero position", heroPos, positionBasicSeats(heroPos, P)});
        field.parts.push_back({"villain position", villanPos, positionBasicSeats(villanPos, P)});
    }
    const bool streetRaised = round < 4 && raiseCounts[round] > 0;
    {
        KeyField& field = add("lastRaise", "the street's last raise", 30, 5);
        const int value = int(field.value);
        field.zeroed = !streetRaised;
        field.meaning = streetRaised ? "three base 3 digits: what the last raiser faced, "
            "seats behind it, callers before it" : "no raise on this street";
        field.parts.push_back({"raiser faced", value / 9,
            streetRaised ? countClamped(value / 9, "raises") + " since the raiser last acted"
                : "no raise", !streetRaised});
        field.parts.push_back({"behind raiser", value / 3 % 3,
            streetRaised ? positionBucket(value / 3 % 3) + " left to act when it was made"
                : "no raise", !streetRaised});
        field.parts.push_back({"callers before", value % 3,
            streetRaised ? countClamped(value % 3, "calls") + " between the pot opening (or the raise before) and it"
                : "no raise", !streetRaised});
    }
    {
        KeyField& field = add("hiddenRaiseSize", "raise before was big", 35, 1);
        const bool reraised = round < 4 && raiseCounts[round] > 1;
        field.zeroed = !reraised;
        field.meaning = !reraised ? "fewer than two raises on this street"
            : field.value ? "the raise before was 1.5x pot or all in" : "the raise before was 0.5x pot";
    }
    {
        KeyField& field = add("hero", "hero's spot", 36, 5);
        const int value = int(field.value);
        field.meaning = "three base 3 digits: seats behind hero, raises hero faces, "
            "callers since the last raise";
        field.parts.push_back({"behind hero", value / 9, positionBucket(value / 9) + " still to act"});
        field.parts.push_back({"raises faced", value / 3 % 3,
            countClamped(value / 3 % 3, "raises") + " since hero last acted"});
        // Engine::play counts a call only once the pot is open, so the opening limp is
        // not one, and an unopened postflop street has none.
        field.parts.push_back({"callers", value % 3, countClamped(value % 3, "calls")
            + " since the pot was opened or last raised"});
    }
    {
        KeyField& field = add("relativePosition", "hero vs villain", 41, 1);
        field.meaning = field.value ? "hero acts after the villain postflop (in position)"
            : "hero acts at or before the villain postflop (out of position)";
    }
    {
        KeyField& field = add("calledTwice", "hero called twice", 42, 1);
        field.meaning = field.value ? "hero has called twice this street, so cannot call again"
            : "hero has called at most once this street";
    }
    {
        KeyField& field = add("boardField", "board class + 169", ACTION_STATE_BITS, BOARD_CLASS_BITS);
        if (round == 0) {
            field.shared = false;
            field.meaning = "preflop: holds each combo's 169 class, so it is per combo";
        } else {
            field.meaning = std::string("stage ") + char('A' + config.metasStage)
                + " board class " + std::to_string(int(field.value) - 169);
        }
    }
    return out;
}
