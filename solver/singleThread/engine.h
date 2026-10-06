#pragma once

#include "solver/config.h"
#include "solver/handAnalysis/handClusterer.h"

struct Engine {
    struct Player {
        std::array<float, 1326> range;
        std::array<float, 1326> rewards;
        float chips;
        uint8_t position;
        uint8_t positionBasic;
        // 3 bits
        uint8_t villanPosition;
        float spr;
        // 2 bits
        float cpr;
        // 2 bits
        bool inHand;
        float bet;
        std::array<uint8_t, 4> lastActions;
        // 4 * 1 bits
        uint8_t raisesFaced;
        // 2 bits
        uint8_t relativePosition;
        // 1 bit
        uint8_t calls;
        // 1 bit

        void set(int position) {
            std::fill(range.begin(), range.end(), 1.0f);
            rewards = {0};
            chips = CHIP_COUNT;
            this->position = position;
            positionBasic = position > 2 ? (3 + (position - 3) / 2) : position;
            villanPosition = 0;
            inHand = true;
            bet = 0;
            for (int i = 0; i < 4; i++) {
                lastActions[i] = 0;
            }
            raisesFaced = 0;
            relativePosition = 0;
            calls = 0;
        }
    };

    std::array<std::array<uint32_t, 8>, 53> binomials;
    inline static std::vector<uint32_t> handScoreTable;
    inline static std::vector<uint16_t> boardClassTable;
    inline static std::vector<uint16_t> boardClassBasicTable;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetasBasic;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetas;
    std::array<int, 1326> preflopHandClasses;
    HandClusterer handClusterer;
    std::array<Player, PLAYER_COUNT> players;
    std::array<int, 52> deck;
    std::array<int, 5> boardCards;
    std::array<bool, 1326> commonRange;
    uint8_t round;
    int heads;
    uint8_t headsBasic;
    // 2 bits
    int firstAction;
    uint8_t action;
    uint8_t actionPosition;
    // 2 bits
    float bet;
    float pot;
    uint16_t boardClass;
    uint16_t boardClassBasic;
    // 15 bits
    std::array<uint8_t, 4> raiseHistory;
    uint8_t raiseHistoryCompressed;
    // 8 bits
    uint8_t raiseSize;
    // 1 bit
    uint8_t raiseStrength;
    // 2 bits
    uint8_t raiseActionPosition;
    // 2 bits
    uint8_t hiddenRaiseSize;
    // 1 bit
    uint8_t hiddenRaiseStrength;
    // 2 bits
    uint8_t hiddenRaiseActionPosition;
    // 2 bits
    uint8_t calls;
    // 2 bits

    uint32_t getHandIndex(std::array<int, 2>& holeCards) {
        std::array<int, 7> handCards = {boardCards[0], boardCards[1], boardCards[2], 
            boardCards[3], boardCards[4], holeCards[0], holeCards[1]};
        std::sort(handCards.begin(), handCards.end(), [](int a, int b) {
            return a < b;
        });
        uint32_t handIndex = 0;
        int q = -1;
        for (int i = 0; i < 7; i++) {
            for (int j = q + 1; j < handCards[i]; j++) {
                handIndex += binomials[51 - j][7 - i - 1];
            }
            q = handCards[i];
        }
        return handIndex;
    }

    uint32_t getBoardIndex() {
        uint32_t boardIndex = 0;
        for (int x = 0; x < boardCards[0]; ++x) {
            boardIndex += binomials[51 - x][2];
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

    Engine() {
        for (int i = 0; i < 53; i++) {
            for (int j = 0; j < 8; j++) {
                if (j == 0) {
                    binomials[i][j] = 1;
                } else if (j > i) {
                    binomials[i][j] = 0;
                } else if (i == j) {
                    binomials[i][j] = 1;
                } else {
                    binomials[i][j] = binomials[i - 1][j - 1] + binomials[i - 1][j];
                }
            }
        }
        for (int i = 0; i < 51; i++) {
            for (int j = i + 1; j < 52; j++) {
                int preflopHandMeta = (i / 4) * (25 - (i / 4)) / 2
                     + (j / 4) + (90 - (i / 4)) * (i % 4 == j % 4);
                preflopHandClasses[i * (101 - i) / 2 + j - 1] = preflopHandMeta;
            }
        }
        std::iota(deck.begin(), deck.end(), 0);
    }

    static void loadTables() {
        static std::once_flag loadFlag;
        std::call_once(loadFlag, [] {
            std::ifstream in("solver/handAnalysis/handScores.bin", std::ios::binary);
            handScoreTable.resize(133784560);
            in.read(reinterpret_cast<char*>(handScoreTable.data()), 
                handScoreTable.size() * sizeof(uint32_t));

            std::ifstream in("solver/handAnalysis/boardClasses.bin", std::ios::binary);
            boardClassTable.resize(60929700);
            in.read(reinterpret_cast<char*>(boardClassTable.data()), 
                boardClassTable.size() * sizeof(uint16_t));

            std::ifstream in("solver/handAnalysis/boardClassesBasic.bin", std::ios::binary);
            boardClassBasicTable.resize(60929700);
            in.read(reinterpret_cast<char*>(boardClassBasicTable.data()), 
                boardClassBasicTable.size() * sizeof(uint16_t));

            std::ifstream in("solver/handAnalysis/handMetasBasic.bin", std::ios::binary);
            uint64_t n, key;
            uint8_t val;
            in.read(reinterpret_cast<char*>(&n), sizeof n);
            handMetasBasic.reserve(n);
            for (uint64_t i = 0; i < n; ++i) {
                in.read(reinterpret_cast<char*>(&key), sizeof key);
                in.read(reinterpret_cast<char*>(&val), sizeof val);
                handMetasBasic.emplace(key, val);
            }

            std::ifstream in("solver/handAnalysis/handMetas.bin", std::ios::binary);
            uint64_t n, key;
            uint8_t val;
            in.read(reinterpret_cast<char*>(&n), sizeof n);
            handMetas.reserve(n);
            for (uint64_t i = 0; i < n; ++i) {
                in.read(reinterpret_cast<char*>(&key), sizeof key);
                in.read(reinterpret_cast<char*>(&val), sizeof val);
                handMetas.emplace(key, val);
            }
        });
    }

    std::uniform_int_distribution<int> cardDist;

    void shuffle(std::mt19937& gen) {
        using param = std::uniform_int_distribution<int>::param_type;
        for (int i = 0; i < 5; i++) {
            std::swap(deck[i], deck[cardDist(gen, param(i, 51))]);
        }
    }

    void deal() {
        boardCards = {deck[0], deck[1], deck[2], deck[3], deck[4]};
        std::sort(boardCards.begin(), boardCards.begin() + 3, [](int a, int b) {
            return a < b;
        });
        std::fill(commonRange.begin(), commonRange.end(), 1);
        for (int i = 0; i < 51; i++) {
            for (int j = i + 1; j < 52; j++) {
                bool legal = true;
                for (int k = 0; k < round + 2; k++) {
                    if (i == deck[k] || j == deck[k]) {
                        commonRange[i * (101 - i) / 2 + j - 1] = 0;
                        break;
                    } 
                }
            }
        }
        for (int i = 0; i < PLAYER_COUNT; i++) {
            players[i].set(i);
        }
        round = 0;
        heads = PLAYER_COUNT;
        headsBasic = heads > 3 ? 2 + (heads - 4) / 2 : heads - 2;
        firstAction = 3 % PLAYER_COUNT;
        action = firstAction;
        actionPosition = PLAYER_COUNT - 1;
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

    std::array<uint64_t, 1326> getState(bool memoryBasic, bool metasBasic) {
        Player& player = players[action];
        Player& villan = players[player.villanPosition];
        std::array<uint64_t, 1326> state = {0};
        uint64_t commonState = 0;
        int shift = 0;
        commonState |= uint64_t(headsBasic & 0x3) << shift;
        shift += 2;
        player.spr = float(player.chips - (bet - player.bet)) / (pot + (bet - player.bet));
        player.cpr = float(bet - player.bet) / (pot - (bet - player.bet));
        if (!memoryBasic) {
            if (player.spr < 0.0005f) {
                if (player.cpr < 0.289f) {
                    commonState |= uint64_t(9 & 0xF) << shift;
                } else if (player.cpr < 0.5f) {
                    commonState |= uint64_t(10 & 0xF) << shift;
                } else if (player.cpr < 0.866f) {
                    commonState |= uint64_t(11 & 0xF) << shift;
                } else if (player.cpr < 1.5f) {
                    commonState |= uint64_t(12 & 0xF) << shift;
                } else if (player.cpr < 2.6f) {
                    commonState |= uint64_t(13 & 0xF) << shift;
                } else if (player.cpr < 4.5f) {
                    commonState |= uint64_t(14 & 0xF) << shift;
                } else {
                    commonState |= uint64_t(15 & 0xF) << shift;
                } 
            } else if (player.spr < 0.289f) {
                commonState |= uint64_t(0 & 0xF) << shift;
            } else if (player.spr < 0.5f) {
                commonState |= uint64_t(1 & 0xF) << shift;
            } else if (player.spr < 0.866f) {
                commonState |= uint64_t(2 & 0xF) << shift;
            } else if (player.spr < 1.5f) {
                commonState |= uint64_t(3 & 0xF) << shift;
            } else if (player.spr < 2.6f) {
                commonState |= uint64_t(4 & 0xF) << shift;
            } else if (player.spr < 4.5f) {
                commonState |= uint64_t(5 & 0xF) << shift;
            } else if (player.spr < 7.8f) {
                commonState |= uint64_t(6 & 0xF) << shift;
            } else if (player.spr < 13.5f) {
                commonState |= uint64_t(7 & 0xF) << shift;
            } else {
                commonState |= uint64_t(8 & 0xF) << shift;
            }
        } else {
            if (player.spr < 0.0005f) {
                if (player.cpr < 0.289f) {
                    commonState |= uint64_t(9 & 0xF) << shift;
                } else if (player.cpr < 0.866f) {
                    commonState |= uint64_t(11 & 0xF) << shift;
                } else if (player.cpr < 2.6f) {
                    commonState |= uint64_t(13 & 0xF) << shift;
                } else {
                    commonState |= uint64_t(15 & 0xF) << shift;
                } 
            } else if (player.spr < 0.289f) {
                commonState |= uint64_t(0 & 0xF) << shift;
            } else if (player.spr < 0.866f) {
                commonState |= uint64_t(2 & 0xF) << shift;
            } else if (player.spr < 2.6f) {
                commonState |= uint64_t(4 & 0xF) << shift;
            } else if (player.spr < 7.8f) {
                commonState |= uint64_t(6 & 0xF) << shift;
            } else {
                commonState |= uint64_t(8 & 0xF) << shift;
            }
        }
        shift += 4;
        commonState |= uint64_t(raiseHistoryCompressed & 0xFF) << shift;
        shift += 8;
        uint64_t heroVillanStory = 0;
        for (int i = 0; i < 4; ++i) {
            heroVillanStory = 3 * heroVillanStory + 2 * player.lastActions[i] + villan.lastActions[i];
        }
        heroVillanStory = 5 * heroVillanStory + player.positionBasic;
        heroVillanStory = 5 * heroVillanStory + villan.positionBasic;
        commonState |= uint64_t(heroVillanStory & 0x7FF) << shift;
        shift += 11;
        uint64_t actionPositionState = 0;
        if (actionPosition < 1) {
            actionPositionState = 3 * actionPosition + 0; 
        } else if (actionPosition < 3) {
            actionPositionState = 3 * actionPosition + 1; 
        } else {
            actionPositionState = 3 * actionPosition + 2; 
        }    
        if (raiseActionPosition < 1) {
            actionPositionState = 3 * raiseActionPosition + 0; 
        } else if (raiseActionPosition < 3) {
            actionPositionState = 3 * raiseActionPosition + 1; 
        } else {
            actionPositionState = 3 * raiseActionPosition + 2; 
        }    
        if (hiddenRaiseActionPosition < 1) {
            actionPositionState = 3 * hiddenRaiseActionPosition + 0; 
        } else if (hiddenRaiseActionPosition < 3) {
            actionPositionState = 3 * hiddenRaiseActionPosition + 1; 
        } else {
            actionPositionState = 3 * hiddenRaiseActionPosition + 2; 
        }    
        commonState |= uint64_t(actionPositionState & 0x1F) << shift;
        shift += 5;
        commonState |= uint64_t((raiseSize > 0) & 0x1) << shift;
        commonState |= uint64_t(std::min(raiseStrength, uint8_t(3)) & 0x3) << (shift + 1);
        shift += 3;
        if (!memoryBasic) {
            commonState |= uint64_t((hiddenRaiseSize > 0) & 0x1) << shift;
            commonState |= uint64_t(std::min(hiddenRaiseStrength, uint8_t(3)) & 0x3) << (shift + 1);
        }
        shift += 3;
        commonState |= uint64_t(std::min(player.raisesFaced, uint8_t(3)) & 0x3) << shift;
        shift += 2;
        commonState |= uint64_t(player.relativePosition & 0x1) << shift;
        shift += 1;
        commonState |= uint64_t((player.calls > 1) & 0x1) << shift;
        shift += 1;
        if (!memoryBasic) {
            if (calls < 2) {
                commonState |= uint64_t(calls & 0x3) << shift;
            } else if (calls < 4) {
                commonState |= uint64_t(2 & 0x3) << shift;
            } else {
                commonState |= uint64_t(3 & 0x3) << shift;
            }
        }
        shift += 2;

        uint64_t metaState;
        for (int i = 0; i < 51; i++) {
            for (int j = i + 1; j < 52; j++) {
                if (commonRange[i * (101 - i) / 2 + j - 1]) {
                    uint32_t handClass = handClusterer.classifyHand(boardCards, {i, j}, round);
                    if (!metasBasic) {
                        uint64_t key = uint64_t(boardClass) | uint64_t(boardClass << 16) | uint64_t(handClass << 32);
                        metaState = (uint64_t(boardClass) | uint64_t(handMetas[key] << 16)) & 0x1FFFFF;
                    } else {
                        uint64_t key = uint64_t(boardClassBasic) | uint64_t(boardClass << 16) | uint64_t(handClass << 32);
                        metaState = (uint64_t(boardClass) | uint64_t(handMetasBasic[key] << 16)) & 0x1FFFFF;
                    }
                    state[i * (101 - i) / 2 + j - 1] = uint64_t(commonState) | uint64_t(metaState << shift);
                } 
            }
        }
        return state;
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
            if (raiseHistory[0] == 0 || player.raisesFaced > 1 || player.calls > 1
                || (calls > 1 && (raiseHistory[0] > 1 || round != 0))) {
                options[1] = false;
            }
        }
        return options;
    }   

    int getAction() {
        return action;
    }

    float getChips() {
        return players[action].chips;
    }

    float getPot() {
        return pot + (bet - players[action].bet);
    }

    std::array<bool, 1326>& getCommonRange() {
        return commonRange;
    }

    std::array<float, 1326>& getRange() {
        return players[action].range;
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
                if ((firstAction + PLAYER_COUNT - 1) % PLAYER_COUNT <= (action + PLAYER_COUNT - 1) % PLAYER_COUNT) {
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
            actionPosition = heads - 1;
            player.lastActions[round] = 1;
            if (round == 0) {
                raiseHistoryCompressed += binomials[9 - raiseHistory[0]][3];
            } else if (round == 1) {
                raiseHistoryCompressed += binomials[8 - raiseHistory[0] - raiseHistory[1]][2];
            } else if (round == 2) {
                raiseHistoryCompressed += 7 - raiseHistory[0] - raiseHistory[1] - raiseHistory[2];
            } else if (round == 3) {
                raiseHistoryCompressed += 1;
            }
            raiseHistory[round]++;
            hiddenRaiseSize = raiseSize;
            hiddenRaiseStrength = raiseStrength;
            hiddenRaiseActionPosition = raiseActionPosition;
            raiseSize = option - 2;
            raiseStrength = player.raisesFaced;
            raiseActionPosition = actionPosition;
            player.raisesFaced = 0;
            for (int i = 0; i < PLAYER_COUNT; i++) {
                if (players[i].inHand && i != action) {
                    players[i].raisesFaced++;
                    players[i].villanPosition = player.position;
                    if ((i + PLAYER_COUNT - 1) % PLAYER_COUNT <= (action + PLAYER_COUNT - 1) % PLAYER_COUNT) {
                        players[i].relativePosition = 0;
                    } else {
                        players[i].relativePosition = 1;
                    }
                }
            }
            calls = 0;
        }
        while (true) {
            action = (action + 1) % PLAYER_COUNT;
            if (action == firstAction) {
                return newRound();
            } else if (players[action].inHand) {
                actionPosition--;
                return true;
            }
        }
    }

    bool newRound() {
        round++;
        if (round == 4) {
            return false;
        } 
        firstAction = 1;
        action = firstAction;
        while (!players[action].inHand) {
            action = (action + 1) % PLAYER_COUNT;
        }
        if (players[action].chips < 0.0005f) {
            return false;
        }
        actionPosition = heads - 1;
        bet = 0;
        boardClass = boardClassTable[getBoardIndex()];
        boardClassBasic = boardClassBasicTable[getBoardIndex()];
        for (int i = 0; i < PLAYER_COUNT; i++) {
            if (players[i].inHand) {
                players[i].bet = 0;
                players[i].calls = 0;
            }
        }
        calls = 0;
        return true;
    }

    std::array<std::array<float, 1326>, PLAYER_COUNT> getRewards() {
        std::array<std::array<float, 1326>, PLAYER_COUNT> rewards = {0};
        for (int i = 0; i < PLAYER_COUNT; i++) {
            if (players[i].inHand) {
                //get expected final chip count for the specifc hand against opponents ranges
                //account for blockers
            } 
        }
        return rewards;
    }
};
