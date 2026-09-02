#pragma once

#include "solver/engine.h"

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

// How many board cards a street has. The replay reaches a round and the board
// must match it exactly, so a flop query with four cards is an error rather than
// a river query with the last card ignored.
inline int roundBoardCards(int round) {
    return round == 0 ? 0 : round + 2;
}

inline const char* OPTION_NAMES[5] = {
    "fold", "call", "raise 0.5x pot", "raise 1.5x pot", "all in"
};

// Why getOptions() turned one option off.
//
// It re-evaluates the same conditions rather than reading the returned mask,
// because the mask says only that an option is off, never which clause did it.
// That makes this a second copy of getOptions()'s logic, so keyEquivalence asserts
// every illegal option gets a non-empty reason at every node it visits - an option
// that falls through every clause here would otherwise be rejected with no
// explanation at all.
//
// Returns "" for an option that is legal.
inline std::string illegalReason(Engine& engine, int option) {
    const Engine::Player& player = engine.players[engine.action];
    const int raises = engine.raiseHistory[0] + engine.raiseHistory[1]
        + engine.raiseHistory[2] + engine.raiseHistory[3];
    auto sprText = [&] { return std::to_string(player.spr); };

    if (option == 0) {
        if (player.cpr < 0.01f) {
            return "there is nothing to call, so folding is not an option here - "
                "check or bet";
        }
        return "";
    }

    if (option == 1) {
        // The call clauses only apply inside this guard; outside it, calling is
        // always legal.
        if (!(player.spr >= 0.01f && raises < 6)) {
            return "";
        }
        // First in source order, matching how the || chain reads.
        if (engine.raiseHistory[0] == 0) {
            return "no preflop limping: the pot is unraised, so every seat must "
                "fold or raise";
        }
        if (player.raisesFaced > 1) {
            return "you are facing a 3-bet or more since you last acted: fold or "
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

    if (player.spr < 0.01f) {
        return "calling is already all-in, so there are no chips behind to raise "
            "with";
    }
    if (raises >= 6) {
        return "the hand has reached its six-raise cap";
    }
    if (raises > 4) {
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

struct ReplayResult {
    bool ok = false;
    std::string error;
    // Names the restriction that turned the option off, when the failure was an
    // illegal option. Empty otherwise.
    std::string reason;
    // Index into the submitted action list, or -1 when the failure is not tied
    // to one action.
    int actionIndex = -1;
    std::array<bool, 5> legal = {};
    bool hasLegal = false;
    // The seat that played the last entry in the action list, and what it played.
    // -1 when the list was empty, so there is no preceding action to describe.
    int lastSeat = -1;
    int lastOption = -1;
};

// Drives the real Engine to the node the action list describes.
//
// The 64 bit state key is the accumulated residue of Engine::play() - nine of its
// fields exist nowhere else - so there is no way to construct a node from a
// snapshot of stacks and pot. Replaying is not an implementation choice, it is
// the only route.
struct Replay {
    Engine engine;
    // The cards the caller actually supplied. Engine::boardCards also holds
    // placeholders for streets the replay has not reached, so combo validity is
    // tested against this rather than against the engine's board.
    std::vector<int> board;

    // deck[0..4] are the board, deck[5..18] the seven pairs of hole cards.
    // Streets not yet reached and every villain hand get placeholder cards drawn
    // from the unused remainder: they are never read into the key, but they must
    // be real, distinct, non clashing cards because newRound() classifies every
    // live player's hand and looks the result up with at().
    void setDeck(const std::vector<int>& boardCards) {
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
        std::iota(engine.deck.begin(), engine.deck.end(), 0);
        for (int i = 0; i < 5; i++) {
            engine.deck[i] = i < int(boardCards.size()) ? boardCards[i] : takeUnused();
        }
        for (int i = 5; i < 2 * PLAYER_COUNT + 5; i++) {
            engine.deck[i] = takeUnused();
        }
    }

    // Replays the option indices in order. Each is checked against getOptions()
    // before it is played: the tree forbids preflop limping, flat calling a
    // 3-bet, a third call on a street, a third cold caller postflop, and any raise
    // past the sixth, so an action list taken from a real hand will hit one of
    // these and has to be told which.
    ReplayResult run(const std::vector<int>& actions, const std::vector<int>& boardCards) {
        ReplayResult result;
        setDeck(boardCards);
        engine.deal();

        for (size_t i = 0; i < actions.size(); i++) {
            int option = actions[i];
            if (option < 0 || option > 4) {
                result.error = "action " + std::to_string(option)
                    + " is not an option index in 0..4";
                result.actionIndex = int(i);
                return result;
            }
            // getState() writes player.spr and player.cpr as a side effect and
            // getOptions() reads them, so this ordering is load bearing.
            engine.getState(METAS_BASIC, MEMORY_BASIC);
            std::array<bool, 5> legal = engine.getOptions();
            if (!legal[option]) {
                result.error = std::string("option ") + std::to_string(option) + " ("
                    + OPTION_NAMES[option] + ") is not legal here";
                result.reason = illegalReason(engine, option);
                result.actionIndex = int(i);
                result.legal = legal;
                result.hasLegal = true;
                return result;
            }
            // Captured before play(), which advances the action pointer.
            result.lastSeat = engine.getAction();
            result.lastOption = option;
            if (!engine.play(option)) {
                result.error = "the hand ended at action " + std::to_string(i)
                    + ", so there is no node to query";
                result.actionIndex = int(i);
                return result;
            }
        }

        int expected = roundBoardCards(engine.round);
        if (int(boardCards.size()) != expected) {
            result.error = "the action list reaches "
                + std::string(engine.round == 0 ? "preflop"
                    : engine.round == 1 ? "the flop"
                    : engine.round == 2 ? "the turn" : "the river")
                + ", which needs " + std::to_string(expected) + " board cards, but "
                + std::to_string(boardCards.size()) + " were given";
            return result;
        }

        engine.getState(METAS_BASIC, MEMORY_BASIC);
        result.legal = engine.getOptions();
        result.hasLegal = true;
        result.ok = true;
        return result;
    }
};
