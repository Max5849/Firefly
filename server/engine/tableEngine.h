#pragma once

// HandClusterer comes through tables.h -> solver/multiHand/engine.h; handClusterer.h
// has no include guard, so it must not be included a second time.
#include "server/engine/tables.h"

// A combo's node within a decision; see getGroup. NO_GROUP marks a combo out of the
// range. The same values explorer.cpp uses.
inline constexpr int GROUP_COUNT = 256;
inline constexpr uint8_t NO_GROUP = 0xFF;

// The game rules of solver/multiHand/engine.h, for any table size, with nothing the
// trainer needs only to learn: no rewards, no showdown, no line odds or card removal
// sums. The server builds one per request's player count and starting stack, replays
// an action list through it, and reads the state key and every seat's range.
//
// setActionState, getOptions, play, beginNewRound and getBoardIndex are the trainer's
// own code with PLAYER_COUNT as P. deal, setCommonRange, setMetaState and applyPolicy
// do what the trainer does for the server's case: the board is given rather than
// shuffled, every combo off the board is classified (not only live ones), and ranges
// are narrowed without the trainer's removal sums. server/tests/keyEquivalence.cpp
// plays this engine in lockstep with Engine at PLAYER_COUNT and asserts the keys,
// options, metas and ranges agree.
template <int P>
struct TableEngine {
    static_assert(P >= 3 && P <= 7, "seat 2 posts the big blind, and positionBasic and "
        "headsBasic fit at most 7 seats");

    struct Player {
        std::array<float, COMBO_COUNT> range;
        float chips;
        bool inHand;
        uint8_t position;
        uint8_t villanPosition;
        float bet;
        uint8_t positionBasic;
        std::array<uint8_t, 4> lastActions;
        float spr;
        float cpr;
        uint8_t raisesFaced;
        uint8_t relativePosition;
        uint8_t calls;

        // As Engine::Player::set, with the stack given.
        void set(int position, float startingChips) {
            std::fill(range.begin(), range.end(), 1.0f);
            chips = startingChips;
            inHand = true;
            this->position = position;
            villanPosition = 0;
            bet = 0;
            positionBasic = position > 2 ? (3 + (position - 3) / 2) : position;
            for (int i = 0; i < 4; i++) {
                lastActions[i] = 0;
            }
            raisesFaced = 0;
            relativePosition = 0;
            calls = 0;
        }
    };

    HandClusterer handClusterer;
    float startingChips = CHIP_COUNT;
    std::array<Player, P> players;
    // The board, with placeholders for streets not yet reached; the flop is sorted, as
    // Engine::deal sorts it.
    std::array<int, 5> boardCards;
    std::array<bool, COMBO_COUNT> commonRange;
    uint64_t actionState;
    uint64_t actionStateBasic;
    std::array<uint64_t, COMBO_COUNT> metaState;
    std::array<uint64_t, COMBO_COUNT> metaStateBasic;
    uint8_t round;
    int heads;
    uint8_t headsBasic;
    int firstAction;
    uint8_t action;
    uint8_t actionPosition;
    float bet;
    float pot;
    uint16_t boardClass;
    uint16_t boardClassBasic;
    std::array<uint8_t, 4> raiseHistory;
    uint8_t raiseHistoryCompressed;
    uint8_t raiseSize;
    uint8_t raiseStrength;
    uint8_t raiseActionPosition;
    uint8_t hiddenRaiseSize;
    uint8_t hiddenRaiseStrength;
    uint8_t hiddenRaiseActionPosition;
    uint8_t calls;

    uint32_t getBoardIndex() {
        uint32_t boardIndex = 0;
        for (int x = 0; x < boardCards[0]; ++x) {
            boardIndex += ServerTables::binomials[51 - x][2];
        }
        for (int x = boardCards[0] + 1; x < boardCards[1]; ++x) {
            boardIndex += 51 - x;
        }
        boardIndex += boardCards[2] - boardCards[1] - 1;
        boardIndex *= 2757;
        if (round == 1) {
            return boardIndex;
        }
        boardIndex += 1 + boardCards[3] * 53;
        if (round == 2) {
            return boardIndex;
        }
        boardIndex += 1 + boardCards[4];
        return boardIndex;
    }

    // Engine::deal, given the five board cards (the server fills streets the hand has
    // not reached with any unused cards; nothing reads them before they are revealed).
    void deal(const std::array<int, 5>& board) {
        for (int i = 0; i < P; i++) {
            players[i].set(i, startingChips);
        }
        boardCards = board;
        std::sort(boardCards.begin(), boardCards.begin() + 3, [](int a, int b) {
            return a < b;
        });
        std::fill(commonRange.begin(), commonRange.end(), 1);
        for (int i = 0; i < COMBO_COUNT; i++) {
            metaStateBasic[i] = ServerTables::preflopHandClasses[i];
            metaState[i] = ServerTables::preflopHandClasses[i];
        }
        round = 0;
        heads = P;
        headsBasic = heads > 3 ? 2 + (heads - 4) / 2 : heads - 2;
        firstAction = 3 % P;
        action = firstAction;
        actionPosition = P - 1;
        bet = 2;
        players[1].chips -= 1;
        players[1].bet = 1;
        players[2].chips -= 2;
        players[2].bet = 2;
        pot = 3;
        raiseHistory = {0};
        raiseHistoryCompressed = 0;
        raiseSize = 0;
        raiseStrength = 0;
        raiseActionPosition = 0;
        hiddenRaiseSize = 0;
        hiddenRaiseStrength = 0;
        hiddenRaiseActionPosition = 0;
        calls = 0;
    }

    void setActionState(bool memoryBasic) {
        Player& player = players[action];
        Player& villan = players[player.villanPosition];
        uint64_t state = 0;
        int shift = 0;
        state |= uint64_t(headsBasic & 0x3) << shift;
        shift += 2;
        player.spr = float(player.chips - (bet - player.bet)) / (pot + (bet - player.bet));
        player.cpr = float(bet - player.bet) / (pot - (bet - player.bet));
        if (!memoryBasic) {
            if (player.spr < 0.0005f) {
                if (player.cpr < 0.289f) {
                    state |= uint64_t(9 & 0xF) << shift;
                } else if (player.cpr < 0.5f) {
                    state |= uint64_t(10 & 0xF) << shift;
                } else if (player.cpr < 0.866f) {
                    state |= uint64_t(11 & 0xF) << shift;
                } else if (player.cpr < 1.5f) {
                    state |= uint64_t(12 & 0xF) << shift;
                } else if (player.cpr < 2.6f) {
                    state |= uint64_t(13 & 0xF) << shift;
                } else if (player.cpr < 4.5f) {
                    state |= uint64_t(14 & 0xF) << shift;
                } else {
                    state |= uint64_t(15 & 0xF) << shift;
                } 
            } else if (player.spr < 0.289f) {
                state |= uint64_t(0 & 0xF) << shift;
            } else if (player.spr < 0.5f) {
                state |= uint64_t(1 & 0xF) << shift;
            } else if (player.spr < 0.866f) {
                state |= uint64_t(2 & 0xF) << shift;
            } else if (player.spr < 1.5f) {
                state |= uint64_t(3 & 0xF) << shift;
            } else if (player.spr < 2.6f) {
                state |= uint64_t(4 & 0xF) << shift;
            } else if (player.spr < 4.5f) {
                state |= uint64_t(5 & 0xF) << shift;
            } else if (player.spr < 7.8f) {
                state |= uint64_t(6 & 0xF) << shift;
            } else if (player.spr < 13.5f) {
                state |= uint64_t(7 & 0xF) << shift;
            } else {
                state |= uint64_t(8 & 0xF) << shift;
            }
        } else {
            if (player.spr < 0.0005f) {
                if (player.cpr < 0.289f) {
                    state |= uint64_t(9 & 0xF) << shift;
                } else if (player.cpr < 0.866f) {
                    state |= uint64_t(11 & 0xF) << shift;
                } else if (player.cpr < 2.6f) {
                    state |= uint64_t(13 & 0xF) << shift;
                } else {
                    state |= uint64_t(15 & 0xF) << shift;
                } 
            } else if (player.spr < 0.289f) {
                state |= uint64_t(0 & 0xF) << shift;
            } else if (player.spr < 0.866f) {
                state |= uint64_t(2 & 0xF) << shift;
            } else if (player.spr < 2.6f) {
                state |= uint64_t(4 & 0xF) << shift;
            } else if (player.spr < 7.8f) {
                state |= uint64_t(6 & 0xF) << shift;
            } else {
                state |= uint64_t(8 & 0xF) << shift;
            }
        }
        shift += 4;
        state |= uint64_t(raiseHistoryCompressed & 0xFF) << shift;
        shift += 8;
        uint64_t heroVillanStory = 0;
        for (int i = 0; i < 4; ++i) {
            heroVillanStory *= 3;
            if (i != round) {
                heroVillanStory += 2 * player.lastActions[i] + !memoryBasic * villan.lastActions[i];
            } else {
                heroVillanStory += player.lastActions[i] + !memoryBasic * villan.lastActions[i];
            }
        }
        heroVillanStory *= 5;
        heroVillanStory += (!memoryBasic || round < 1) * player.positionBasic;
        heroVillanStory *= 5;
        heroVillanStory += (!memoryBasic || round < 1) * villan.positionBasic;
        state |= uint64_t(heroVillanStory & 0x7FF) << shift;
        shift += 11;
        uint64_t actionPositionState = 0;
        actionPositionState *= 3;
        actionPositionState += (actionPosition > 0) + (actionPosition > 2);
        actionPositionState *= 3;
        actionPositionState += (raiseActionPosition > 0) + (raiseActionPosition > 2);
        actionPositionState *= 3;
        actionPositionState += !memoryBasic * ((hiddenRaiseActionPosition > 0) + (hiddenRaiseActionPosition > 2));
        state |= uint64_t(actionPositionState & 0x1F) << shift;
        shift += 5;
        state |= uint64_t((raiseSize > 0) & 0x1) << shift;
        state |= uint64_t(std::min(raiseStrength, uint8_t(3)) & 0x3) << (shift + 1);
        shift += 3;
        if (!memoryBasic) {
            state |= uint64_t((hiddenRaiseSize > 0) & 0x1) << shift;
            state |= uint64_t(std::min(hiddenRaiseStrength, uint8_t(3)) & 0x3) << (shift + 1);
        }
        shift += 3;
        state |= uint64_t(std::min(player.raisesFaced, uint8_t(3)) & 0x3) << shift;
        shift += 2;
        state |= uint64_t(player.relativePosition & 0x1) << shift;
        shift += 1;
        state |= uint64_t((player.calls > 1) & 0x1) << shift;
        shift += 1;
        if (!memoryBasic) {
            if (calls < 2) {
                state |= uint64_t(calls & 0x3) << shift;
            } else if (calls < 4) {
                state |= uint64_t(2 & 0x3) << shift;
            } else {
                state |= uint64_t(3 & 0x3) << shift;
            }
        }
        shift += 2;
        if (!memoryBasic) {
            actionState = state;
        } else {
            actionStateBasic = state;
        }
    }

    std::array<bool, 5> getOptions() {
        std::array<bool, 5> options = {true, true, false, false, false};
        Player& player = players[action];
        if (player.cpr < 0.0005f) {
            options[0] = false;
        } 
        int raises = raiseHistory[0] + raiseHistory[1] + raiseHistory[2] + raiseHistory[3];
        if (player.spr >= 0.0005f && raises < 6) {
            if (raises > 4) {
                options[4] = true;
            } else {
                if (player.spr < 7.8f) {
                    options[4] = true;
                } 
                if (player.spr >= 2.6f) {
                    options[3] = true;
                }
                if (player.spr >= 0.866f) {
                    options[2] = true;
                }
            }
            if (raiseHistory[0] == 0 || player.raisesFaced > 2 || player.calls > 1
                || (calls > 1 && (raiseHistory[0] > 1 || round != 0))) {
                options[1] = false;
            }
        }
        return options;
    }   

    uint64_t getActionState(bool memoryBasic) const {
        return !memoryBasic ? actionState : actionStateBasic;
    }

    const std::array<uint64_t, COMBO_COUNT>& getMetaState(bool metasBasic) const {
        return !metasBasic ? metaState : metaStateBasic;
    }

    // The acting player's total bet on this street once option is played, without
    // playing it: unchanged for a fold, the table bet for a call or check, and for a
    // raise the call plus half or one and a half of the pot after the call, or every
    // chip left. The same float operations play() performs.
    float betTo(int option) const {
        const Player& player = players[action];
        if (option == 0) {
            return player.bet;
        }
        if (option == 1) {
            return bet;
        }
        const float potAfterCall = pot + (bet - player.bet);
        const float chipsAfterCall = player.chips - (bet - player.bet);
        if (option == 2) {
            return bet + 0.5f * potAfterCall;
        }
        if (option == 3) {
            return bet + 1.5f * potAfterCall;
        }
        return bet + chipsAfterCall;
    }

    bool play(int option) {
        Player& player = players[action];
        if (option == 0) {
            player.inHand = false;
            heads--;
            if (heads == 1) {
                return false;        
            }
            headsBasic = heads > 3 ? 2 + (heads - 4) / 2 : heads - 2;
        } else if (option == 1 && raiseHistory[round] != 0) {
            player.chips -= (bet - player.bet);
            pot += (bet - player.bet);
            player.bet = bet;
            player.lastActions[round] = 0;
            player.raisesFaced = 0;
            if (calls == 0) {
                players[firstAction].villanPosition = player.position;
                if ((firstAction + P - 1) % P <= (action + P - 1) % P) {
                    players[firstAction].relativePosition = 0;
                } else {
                    players[firstAction].relativePosition = 1;
                }
            }
            player.calls++;
            calls++;
        } else if (option == 2 || option == 3 || option == 4) {
            player.chips -= (bet - player.bet);
            pot += (bet - player.bet);
            player.bet = bet;
            if (option == 2) {
                bet = player.bet + 0.5f * pot;
            } else if (option == 3) {
                bet = player.bet + 1.5f * pot;
            } else if (option == 4) {
                bet = player.bet + player.chips;
            }
            player.chips -= (bet - player.bet);
            pot += (bet - player.bet);
            player.bet = bet;
            firstAction = action;
            hiddenRaiseActionPosition = raiseActionPosition;
            raiseActionPosition = actionPosition;
            actionPosition = heads - 1;
            player.lastActions[round] = 1;
            if (round == 0) {
                raiseHistoryCompressed += ServerTables::binomials[9 - raiseHistory[0]][3];
            } else if (round == 1) {
                raiseHistoryCompressed += ServerTables::binomials[8 - raiseHistory[0] - raiseHistory[1]][2];
            } else if (round == 2) {
                raiseHistoryCompressed += 7 - raiseHistory[0] - raiseHistory[1] - raiseHistory[2];
            } else if (round == 3) {
                raiseHistoryCompressed += 1;
            }
            raiseHistory[round]++;
            hiddenRaiseSize = raiseSize;
            hiddenRaiseStrength = raiseStrength;
            raiseSize = option - 2;
            raiseStrength = player.raisesFaced;
            player.raisesFaced = 0;
            for (int i = 0; i < P; i++) {
                if (players[i].inHand && i != action) {
                    players[i].raisesFaced++;
                    players[i].villanPosition = player.position;
                    if ((i + P - 1) % P <= (action + P - 1) % P) {
                        players[i].relativePosition = 0;
                    } else {
                        players[i].relativePosition = 1;
                    }
                }
            }
            calls = 0;
        }

        while (true) {
            action = (action + 1) % P;
            if (action == firstAction) {
                return false;
            } else if (players[action].inHand) {
                actionPosition--;
                return true;
            }
        }
    }

    // Engine::setCommonRange: every combo holding a revealed board card is out.
    void setCommonRange() {
        for (int i = 0; i < 51; i++) {
            for (int j = i + 1; j < 52; j++) {
                for (int k = 0; k < round + 2; k++) {
                    if (i == boardCards[k] || j == boardCards[k]) {
                        commonRange[holePairIndex(i, j)] = 0;
                        break;
                    }
                }
            }
        }
    }

    // Engine::setMetaState for every combo off the board, live in some range or not:
    // the server shows the strategy of hands the line has ruled out too. For a live
    // combo the result is the trainer's.
    void setMetaState(bool needFull, bool needBasic) {
        uint32_t boardIndex = getBoardIndex();
        boardClass = ServerTables::boardClassTable[boardIndex];
        boardClassBasic = ServerTables::boardClassBasicTable[boardIndex];
        for (int i = 0; i < 51; i++) {
            for (int j = i + 1; j < 52; j++) {
                int index = holePairIndex(i, j);
                if (!commonRange[index]) {
                    continue;
                }
                uint32_t handClass = handClusterer.classifyHand(boardCards, {i, j}, round);
                if (needFull) {
                    uint64_t key = uint64_t(boardClass) | uint64_t(handClass) << 16;
                    metaState[index] = (uint64_t(boardClass + 169)
                        | uint64_t(ServerTables::handMetas.at(key)) << 16) & 0x3FFFFF;
                }
                if (needBasic) {
                    uint64_t key = uint64_t(boardClassBasic) | uint64_t(handClass) << 16;
                    metaStateBasic[index] = (uint64_t(boardClassBasic + 169)
                        | uint64_t(ServerTables::handMetasBasic.at(key)) << 16) & 0x3FFFFF;
                }
            }
        }
    }

    // Engine::getGroup: the 169 class preflop, the hand cluster postflop.
    uint8_t getGroup(bool metasBasic, int combo) const {
        uint64_t meta = getMetaState(metasBasic)[combo];
        return uint8_t(round == 0 ? meta : meta >> 16);
    }

    // The range update in Explorer::chooseRangeOption after a seat played option: every
    // combo with a group takes its group's policy probability for the option, then any
    // combo under RANGE_FLOOR times the range's summed reach is dropped. The trainer
    // rebuilds its card removal sums in the same pass; the server has none.
    void applyPolicy(int seat, const std::array<uint8_t, COMBO_COUNT>& groups,
            const std::array<std::array<double, 5>, GROUP_COUNT>& policy, int option) {
        std::array<float, COMBO_COUNT>& range = players[seat].range;
        double rangeSum = 0;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            const uint8_t group = groups[combo];
            if (group == NO_GROUP) {
                continue;
            }
            range[combo] *= float(policy[group][option]);
            rangeSum += range[combo];
        }
        const double floor = RANGE_FLOOR * rangeSum;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (groups[combo] != NO_GROUP && range[combo] < floor) {
                range[combo] = 0.0f;
            }
        }
    }

    bool beginNewRound() {
        if (heads == 1) {
            return false;
        }
        round++;
        if (round == 4) {
            return false;
        } 
        firstAction = 1;
        action = firstAction;
        while (!players[action].inHand) {
            action = (action + 1) % P;
        }
        if (players[action].chips < 0.0005f) {
            return false;
        }
        actionPosition = heads - 1;
        bet = 0;
        for (int i = 0; i < P; i++) {
            if (players[i].inHand) {
                players[i].bet = 0;
                players[i].calls = 0;
            }
        }
        calls = 0;
        return true;
    }

};
