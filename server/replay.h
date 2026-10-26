#pragma once

#include "server/engine/tableEngine.h"
#include "server/strategyStore.h"

// library.h leans on transitive includes that libc++ happens to provide and
// libstdc++ does not, so the headers this file actually uses are named here.
// The build target is Linux/GCC; the development machine is macOS/clang.
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

// Cards are rank * 4 + suit with rank 0 = ace descending to 12 = deuce, exactly
// as handAnalysis encodes them. The suit letters are this API's own convention:
// the abstraction only ever counts suits, so any consistent mapping serves.
inline constexpr char CARD_RANKS[14] = "AKQJT98765432";
inline constexpr char CARD_SUITS[5] = "cdhs";

inline std::string cardToString(int card) {
    return std::string{CARD_RANKS[card / 4], CARD_SUITS[card % 4]};
}

inline std::string boardToString(const std::vector<int>& cards) {
    std::string text;
    for (int card : cards) {
        text += cardToString(card);
    }
    return text;
}

// Returns -1 for anything that is not a two character rank/suit pair.
inline int parseCard(char rank, char suit) {
    const char* rankAt = std::find(CARD_RANKS, CARD_RANKS + 13, std::toupper(rank));
    const char* suitAt = std::find(CARD_SUITS, CARD_SUITS + 4, std::tolower(suit));
    if (rankAt == CARD_RANKS + 13 || suitAt == CARD_SUITS + 4) {
        return -1;
    }
    return int(rankAt - CARD_RANKS) * 4 + int(suitAt - CARD_SUITS);
}

// Accepts "As7h2d", "As 7h 2d" and "As,7h,2d". Throws with a readable message on
// an unparseable card, a duplicate, or more than five cards.
inline std::vector<int> parseBoard(const std::string& text) {
    std::vector<int> cards;
    for (size_t i = 0; i < text.size(); ) {
        char c = text[i];
        if (c == ' ' || c == ',' || c == '\t') {
            i++;
            continue;
        }
        if (i + 1 >= text.size()) {
            throw std::runtime_error("trailing character '" + std::string{c} + "' in board");
        }
        int card = parseCard(text[i], text[i + 1]);
        if (card < 0) {
            throw std::runtime_error("unrecognised card '" + text.substr(i, 2)
                + "' in board (expected a rank in AKQJT98765432 and a suit in cdhs)");
        }
        if (std::find(cards.begin(), cards.end(), card) != cards.end()) {
            throw std::runtime_error("duplicate card '" + cardToString(card) + "' in board");
        }
        cards.push_back(card);
        i += 2;
    }
    if (cards.size() > 5) {
        throw std::runtime_error("board has " + std::to_string(cards.size())
            + " cards, at most 5 are possible");
    }
    return cards;
}

// How many board cards a street has.
inline int roundBoardCards(int round) {
    return round == 0 ? 0 : round + 2;
}

inline const char* STREET_NAMES[4] = {"preflop", "flop", "turn", "river"};

inline const char* OPTION_NAMES[5] = {
    "fold", "call", "raise 0.5x pot", "raise 1.5x pot", "all in"
};

// Seat 0 is the button and 1, 2 the blinds; the seats after them take the names of the
// last positions before the button, so a 4 seat table has a CO and a 7 seat table
// runs UTG, LJ, HJ, CO.
inline std::vector<std::string> seatNames(int players) {
    static const char* LATE[4] = {"UTG", "LJ", "HJ", "CO"};
    std::vector<std::string> names = {"BTN", "SB", "BB"};
    for (int i = 4 - (players - 3); i < 4; i++) {
        names.push_back(LATE[i]);
    }
    return names;
}

// The name an option goes by at the table: check rather than call with nothing to
// call, and bet rather than raise on an unopened postflop street. Preflop the big
// blind is the bet, so every preflop raise is a raise.
inline std::string actionName(int option, float toCall, int round, int streetRaises) {
    if (option == 0) {
        return "fold";
    }
    if (option == 1) {
        return toCall > 0.0f ? "call" : "check";
    }
    if (option == 4) {
        return "all in";
    }
    return (round > 0 && streetRaises == 0) ? "bet" : "raise";
}

// The inverse temperature a request gets when it names none: solver/singleHand's
// INV_TEMP, ln(2^20), which the strategy files are trained at. (The INV_TEMP in scope
// is multiHand's.)
inline const double DEFAULT_INV_TEMP = 20 * std::log(2.0);

// The policy over the legal options, from a record's four weights, at inverse
// temperature invTemp per pot.
//
// A record's weights are each option's return from the decision on as a share of the
// pot after calling, relative to folding, whose value is the exact 0 - so fold draws
// exp(0) = 1. At INV_TEMP this is the softmax the trainers play by.
inline std::array<double, 5> policyFromWeights(const std::array<float, 4>& weights,
        const std::array<bool, 5>& legal, double invTemp) {
    std::array<double, 5> draws = {};
    double sum = 0;
    for (int j = 0; j < 5; j++) {
        if (legal[j]) {
            double ev = j == 0 ? 0.0 : double(weights[j - 1]);
            draws[j] = std::exp(std::clamp(invTemp * ev, -MAX_EXP_ARG, MAX_EXP_ARG));
        }
        sum += draws[j];
    }
    for (int j = 0; j < 5; j++) {
        draws[j] = sum > 0 ? draws[j] / sum : 0.0;
    }
    return draws;
}

// Why getOptions() turned one option off.
//
// It re-evaluates the same conditions rather than reading the returned mask,
// because the mask says only that an option is off, never which clause did it.
// That makes this a second copy of getOptions()'s logic, so keyEquivalence asserts
// every illegal option gets a non-empty reason at every node it visits.
//
// Returns "" for an option that is legal. Reads spr and cpr, so setActionState()
// must have run at this node.
template <int P>
std::string illegalReason(const TableEngine<P>& engine, int option) {
    const auto& player = engine.players[engine.action];
    const int raises = engine.raiseHistory[0] + engine.raiseHistory[1]
        + engine.raiseHistory[2] + engine.raiseHistory[3];
    auto sprText = [&] { return std::to_string(player.spr); };

    if (option == 0) {
        if (player.cpr < 0.0005f) {
            return "there is nothing to call, so folding is not an option here - "
                "check or bet";
        }
        return "";
    }

    if (option == 1) {
        // The call clauses only apply inside this guard; outside it, calling is
        // always legal.
        if (!(player.spr >= 0.0005f && raises < 6)) {
            return "";
        }
        // First in source order, matching how the || chain reads.
        if (engine.raiseHistory[0] == 0) {
            return "no preflop limping: the pot is unraised, so every seat must "
                "fold or raise";
        }
        if (player.raisesFaced > 2) {
            return "you are facing three or more raises since you last acted: fold or "
                "re-raise, no flat call";
        }
        if (player.calls > 1) {
            return "no third call on the same street";
        }
        if (engine.calls > 1 && (engine.raiseHistory[0] > 1 || engine.round != 0)) {
            return "no third cold-caller in a 3-bet-plus preflop pot or on any "
                "postflop street";
        }
        return "";
    }

    if (player.spr < 0.0005f) {
        return "calling is already all-in, so there are no chips behind to raise "
            "with";
    }
    if (raises >= 6) {
        return "the hand has reached its six-raise cap";
    }
    if (raises > 4) {
        if (option == 4) {
            return "";
        }
        return "the sixth raise can only be all-in";
    }
    if (option == 2 && player.spr < 0.866f) {
        return "the stack-to-pot ratio is " + sprText()
            + ", below the 0.866 needed to raise half pot";
    }
    if (option == 3 && player.spr < 2.6f) {
        return "the stack-to-pot ratio is " + sprText()
            + ", below the 2.6 needed to raise one and a half pot";
    }
    if (option == 4 && player.spr >= 7.8f) {
        return "the stack-to-pot ratio is " + sprText()
            + ", at or above the 7.8 beyond which an all-in is not offered";
    }
    return "";
}

// One line of the hand as the table saw it.
struct HistoryEntry {
    enum Kind { Post, Action, Street };
    Kind kind = Action;
    int seat = -1;
    int option = -1;
    std::string name;
    int round = 0;
    // Post and Action: the seat's total in front on the street afterwards, the chips
    // the action put in, and the stack left.
    float betTo = 0;
    float adds = 0;
    float stack = 0;
    // Street: the board once it is dealt.
    std::string board;
};

struct ReplayResult {
    bool ok = false;
    // Machine readable: badAction, illegal, handEnded, needsBoard, boardMismatch.
    std::string code;
    std::string error;
    // Names the restriction that turned the option off, when the failure was an
    // illegal option. Empty otherwise.
    std::string reason;
    // Index into the submitted action list, or -1 when the failure is not tied
    // to one action.
    int actionIndex = -1;
    std::array<bool, 5> legal = {};
    bool hasLegal = false;
    // needsBoard: the street the action list reached and the board cards it needs.
    int needRound = 0;
    // handEnded: "fold" when one seat is left, "showdown" when the board runs out.
    std::string endedBy;
};

// Groups and combos a range lost along the line because their node had no record.
struct ReplayDrops {
    int decisions = 0;
    int groups = 0;
    int combos = 0;
};

// Drives a TableEngine to the node an action list describes, ranges included.
//
// The 64 bit state key is the accumulated residue of Engine::play(), so there is no way
// to construct a node from a snapshot of stacks and pot: replaying is the only route.
// And a range is the product of its seat's own policy along the line, so every past
// decision is looked up in the strategy and narrowed exactly as the trainer narrows it
// (TableEngine::applyPolicy), with each decision's policy a softmax over the stored
// pot-relative weights at the request's inverse temperature (policyFromWeights); at
// INV_TEMP these are training's ranges. A node the
// strategy has no record of drops the combos that reach it.
template <int P>
struct Replay {
    TableEngine<P> engine;
    StrategyConfig config;
    // The inverse temperature per chip every past decision's policy is taken at.
    double invTemp = DEFAULT_INV_TEMP;
    // The cards the caller actually supplied. Engine::boardCards also holds placeholders
    // for streets the replay has not reached.
    std::vector<int> board;
    std::vector<HistoryEntry> history;
    ReplayDrops drops;
    // Scratch for narrowing.
    std::array<uint8_t, COMBO_COUNT> groups;
    std::array<std::array<double, 5>, GROUP_COUNT> policy;

    // The five cards deal() takes. Streets not yet reached get placeholder cards drawn
    // from the unused remainder: they are never read into the key, but deal() sorts
    // the flop and they must be real, distinct cards.
    std::array<int, 5> fullBoard(const std::vector<int>& boardCards) {
        board = boardCards;
        std::array<bool, 52> used = {};
        for (int card : boardCards) {
            used[card] = true;
        }
        int next = 0;
        auto takeUnused = [&] {
            while (used[next]) {
                next++;
            }
            used[next] = true;
            return next;
        };
        std::array<int, 5> cards;
        for (int i = 0; i < 5; i++) {
            cards[i] = i < int(boardCards.size()) ? boardCards[i] : takeUnused();
        }
        return cards;
    }

    // Narrows the acting seat's range by the option it played: each live combo takes
    // its node's probability for the option at invTemp. Must follow setActionState()
    // and come before play().
    void narrow(const StrategyStore& store, const std::array<bool, 5>& legal, int option) {
        const int seat = engine.action;
        const auto& range = engine.players[seat].range;
        const uint64_t actionState = engine.getActionState(config.memoryBasic);
        const auto& meta = engine.getMetaState(config.metasBasic);
        std::array<int8_t, GROUP_COUNT> seen;
        seen.fill(-1);
        groups.fill(NO_GROUP);
        bool dropped = false;
        StrategyRecord record;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (!engine.commonRange[combo] || !(range[combo] > 0.0f)) {
                continue;
            }
            const uint8_t group = engine.getGroup(config.metasBasic, combo);
            groups[combo] = group;
            if (seen[group] < 0) {
                const uint64_t key = actionState | meta[combo] << 42;
                seen[group] = store.lookup(key, record) ? 1 : 0;
                if (seen[group] == 1) {
                    policy[group] = policyFromWeights(record.weights, legal, invTemp);
                } else {
                    policy[group].fill(0.0);
                    drops.groups++;
                    dropped = true;
                }
            }
            if (seen[group] == 0) {
                drops.combos++;
            }
        }
        drops.decisions += dropped;
        engine.applyPolicy(seat, groups, policy, option);
    }

    // Replays the option indices in order. Each is checked against getOptions() before
    // it is played. store == nullptr replays the action state alone and leaves every
    // range at 1.
    ReplayResult run(const StrategyConfig& setup, const std::vector<int>& actions,
            const std::vector<int>& boardCards, const StrategyStore* store,
            double temperature = DEFAULT_INV_TEMP) {
        ReplayResult result;
        config = setup;
        invTemp = temperature;
        history.clear();
        drops = {};
        engine.startingChips = float(config.chips);
        engine.deal(fullBoard(boardCards));
        for (int seat = 1; seat <= 2; seat++) {
            HistoryEntry post;
            post.kind = HistoryEntry::Post;
            post.seat = seat;
            post.name = seat == 1 ? "small blind" : "big blind";
            post.betTo = engine.players[seat].bet;
            post.adds = engine.players[seat].bet;
            post.stack = engine.players[seat].chips;
            history.push_back(post);
        }

        for (size_t i = 0; i < actions.size(); i++) {
            int option = actions[i];
            if (option < 0 || option > 4) {
                result.code = "badAction";
                result.error = "action " + std::to_string(option)
                    + " is not an option index in 0..4";
                result.actionIndex = int(i);
                return result;
            }
            // setActionState() writes player.spr and player.cpr and getOptions() reads
            // them, so this ordering is load bearing.
            engine.setActionState(config.memoryBasic);
            std::array<bool, 5> legal = engine.getOptions();
            if (!legal[option]) {
                result.code = "illegal";
                result.error = std::string("option ") + std::to_string(option) + " ("
                    + OPTION_NAMES[option] + ") is not legal here";
                result.reason = illegalReason(engine, option);
                result.actionIndex = int(i);
                result.legal = legal;
                result.hasLegal = true;
                return result;
            }
            if (store != nullptr) {
                narrow(*store, legal, option);
            }

            const int seat = engine.action;
            auto& player = engine.players[seat];
            HistoryEntry entry;
            entry.seat = seat;
            entry.option = option;
            entry.round = engine.round;
            entry.name = actionName(option, engine.bet - player.bet, engine.round,
                engine.raiseHistory[engine.round]);
            entry.betTo = engine.betTo(option);
            const float stackBefore = player.chips;
            const bool more = engine.play(option);
            entry.adds = stackBefore - player.chips;
            entry.stack = player.chips;
            history.push_back(entry);
            if (more) {
                continue;
            }

            if (!engine.beginNewRound()) {
                result.code = "handEnded";
                result.endedBy = engine.heads == 1 ? "fold" : "showdown";
                result.error = std::string("the hand ended at action ") + std::to_string(i)
                    + (engine.heads == 1 ? ", when every other seat had folded"
                        : ", when the betting finished and the board runs out to showdown")
                    + ", so there is no node to query";
                result.actionIndex = int(i);
                return result;
            }
            const int needed = roundBoardCards(engine.round);
            if (int(boardCards.size()) < needed) {
                result.code = "needsBoard";
                result.needRound = engine.round;
                result.error = std::string("the action list reaches the ")
                    + STREET_NAMES[engine.round] + ", which needs " + std::to_string(needed)
                    + " board cards, but " + std::to_string(boardCards.size()) + " were given";
                result.actionIndex = int(i);
                return result;
            }
            engine.setCommonRange();
            engine.setMetaState(!config.metasBasic, config.metasBasic);
            HistoryEntry street;
            street.kind = HistoryEntry::Street;
            street.round = engine.round;
            street.name = STREET_NAMES[engine.round];
            street.board = boardToString(std::vector<int>(boardCards.begin(),
                boardCards.begin() + needed));
            history.push_back(street);
        }

        const int expected = roundBoardCards(engine.round);
        if (int(boardCards.size()) != expected) {
            result.code = "boardMismatch";
            result.error = std::string("the action list reaches ")
                + (engine.round == 0 ? "preflop" : std::string("the ") + STREET_NAMES[engine.round])
                + ", which needs " + std::to_string(expected) + " board cards, but "
                + std::to_string(boardCards.size()) + " were given";
            return result;
        }

        engine.setActionState(config.memoryBasic);
        result.legal = engine.getOptions();
        result.hasLegal = true;
        result.ok = true;
        return result;
    }

    // The board cards revealed at the node.
    std::vector<int> revealed() const {
        return std::vector<int>(board.begin(), board.begin() + roundBoardCards(engine.round));
    }
};
