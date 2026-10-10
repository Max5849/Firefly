#pragma once

#include "solver/singleHand/config.h"
#include "solver/handAnalysis/handClusterer.h"

struct Engine {
    struct Player {
        std::array<int, 2> holeCards;
        uint64_t metaState;
        uint64_t metaStateBasic;
        uint32_t handScore;
        // The importance ratio of the line: the product, over every decision made while
        // this seat was in the hand, of the chosen option's INV_TEMP probability over its
        // sampled probability.
        double ratio;
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

        void set(std::array<int, 2> holeCards, uint64_t preflopHandClass, int position) {
            this->holeCards = holeCards;
            metaState = preflopHandClass;
            metaStateBasic = preflopHandClass;
            handScore = 0;
            ratio = 1.0;
            chips = CHIP_COUNT;
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

    inline static std::array<std::array<uint32_t, 8>, 53> binomials;
    inline static std::vector<uint32_t> handScoreTable;
    inline static std::vector<uint16_t> boardClassTable;
    inline static std::vector<uint16_t> boardClassBasicTable;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetasBasic;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetas;
    inline static std::array<uint64_t, 1326> preflopHandClasses;
    HandClusterer handClusterer;
    std::array<Player, PLAYER_COUNT> players;
    std::array<int, 52> deck;
    std::array<int, 5> boardCards;
    uint64_t actionState;
    uint64_t actionStateBasic;
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
        std::iota(deck.begin(), deck.end(), 0);
    }

    static void loadTable(const std::string& path, std::vector<uint16_t>& table) {
        table.resize(60929700);
        uint64_t bytes = table.size() * sizeof(uint16_t);
        if (fileSize(path) != bytes) {
            throw std::runtime_error(path + " has an unexpected size");
        }
        std::ifstream file = openRead(path);
        readExact(file, table.data(), bytes, path);
    }

    static void loadHandMetas(const std::string& path,
            ankerl::unordered_dense::map<uint64_t, uint8_t>& out) {
        std::vector<char> data = readWholeFile(path);
        if (data.size() < sizeof(uint64_t)) {
            throw std::runtime_error(path + " is truncated");
        }
        uint64_t size;
        std::memcpy(&size, data.data(), sizeof(size));
        if (data.size() != sizeof(uint64_t) + size * 9) {
            throw std::runtime_error(path + " has an unexpected size");
        }
        out.reserve(size);
        const char* cursor = data.data() + sizeof(uint64_t);
        for (uint64_t i = 0; i < size; i++) {
            uint64_t key;
            std::memcpy(&key, cursor, sizeof(key));
            out.emplace(key, uint8_t(cursor[8]));
            cursor += 9;
        }
    }

    static void loadTables() {
        static std::once_flag loadFlag;
        std::call_once(loadFlag, [] {
            const std::string handScorePath = "solver/handAnalysis/handScores.bin";
            handScoreTable.resize(133784560);
            uint64_t handScoreBytes = handScoreTable.size() * sizeof(uint32_t);
            if (fileSize(handScorePath) != handScoreBytes) {
                throw std::runtime_error(handScorePath + " has an unexpected size");
            }
            std::ifstream handScoreFile = openRead(handScorePath);
            readExact(handScoreFile, handScoreTable.data(), handScoreBytes, handScorePath);

            loadTable("solver/handAnalysis/boardClasses.bin", boardClassTable);
            loadTable("solver/handAnalysis/boardClassesBasic.bin", boardClassBasicTable);
            if (*std::max_element(boardClassTable.begin(), boardClassTable.end()) + 169 > 0xFFFF
                || *std::max_element(boardClassBasicTable.begin(), boardClassBasicTable.end()) + 169 > 0xFFFF) {
                throw std::runtime_error("board class + 169 overflows the 16-bit key field");
            }

            loadHandMetas("solver/handAnalysis/handMetasBasic.bin", handMetasBasic);
            loadHandMetas("solver/handAnalysis/handMetas.bin", handMetas);

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
        });
    }

    std::uniform_int_distribution<int> cardDist;
    void shuffle(std::mt19937& gen) {
        using param = std::uniform_int_distribution<int>::param_type;
        for (int i = 0; i < 2 * PLAYER_COUNT + 5; i++) {
            std::swap(deck[i], deck[cardDist(gen, param(i, 51))]);
        }
    }

    void deal() {
        boardCards = {deck[0], deck[1], deck[2], deck[3], deck[4]};
        std::sort(boardCards.begin(), boardCards.begin() + 3, [](int a, int b) {
            return a < b;
        });
        for (int i = 0; i < PLAYER_COUNT; i++) {
            std::array<int, 2> holeCards = {deck[2 * i + 5], deck[2 * i + 6]};
            std::sort(holeCards.begin(), holeCards.end(), [](int a, int b) {
                return a < b;
            });
            players[i].set(holeCards,
                preflopHandClasses[holePairIndex(holeCards[0], holeCards[1])], i);
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
                heroVillanStory += 2 * player.lastActions[i] + villan.lastActions[i];
            } else {
                heroVillanStory += player.lastActions[i] + villan.lastActions[i];
            }
        }
        heroVillanStory *= 5;
        heroVillanStory += (!memoryBasic || round < 2) * player.positionBasic;
        heroVillanStory *= 5;
        heroVillanStory += (!memoryBasic || round < 2) * villan.positionBasic;
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

    double& getRatio(int action) {
        return players[action].ratio;
    }

    int getHoleCombo(int action) {
        return holePairIndex(players[action].holeCards[0], players[action].holeCards[1]);
    }

    uint64_t getActionState(bool memoryBasic) {
        return !memoryBasic ? actionState : actionStateBasic;
    }

    // The acting player's meta state, the high part of its key.
    uint64_t getMetaState(bool metasBasic) {
        return !metasBasic ? players[action].metaState : players[action].metaStateBasic;
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
            hiddenRaiseActionPosition = raiseActionPosition;
            raiseActionPosition = actionPosition;
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
            raiseSize = option - 2;
            raiseStrength = player.raisesFaced;
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
                return false;
            } else if (players[action].inHand) {
                actionPosition--;
                return true;
            }
        }
    }

    // Classifies each live player's hand once and fills whichever meta states are
    // needed.
    void setMetaState(bool needFull, bool needBasic) {
        uint32_t boardIndex = getBoardIndex();
        boardClass = boardClassTable[boardIndex];
        boardClassBasic = boardClassBasicTable[boardIndex];
        for (int p = 0; p < PLAYER_COUNT; p++) {
            Player& player = players[p];
            if (!player.inHand) {
                continue;
            }
            uint32_t handClass = handClusterer.classifyHand(boardCards, player.holeCards, round);
            uint64_t key = uint64_t(boardClass) << 16 | uint64_t(handClass) << 32;
            if (needFull) {
                player.metaState = (uint64_t(boardClass + 169)
                    | uint64_t(handMetas.at(key)) << 16) & 0x3FFFFF;
            }
            if (needBasic) {
                player.metaStateBasic = (uint64_t(boardClassBasic + 169)
                    | uint64_t(handMetasBasic.at(key)) << 16) & 0x3FFFFF;
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
            action = (action + 1) % PLAYER_COUNT;
        }
        if (players[action].chips < 0.0005f) {
            return false;
        }
        actionPosition = heads - 1;
        bet = 0;
        for (int i = 0; i < PLAYER_COUNT; i++) {
            if (players[i].inHand) {
                players[i].bet = 0;
                players[i].calls = 0;
            }
        }
        calls = 0;
        return true;
    }

    // Pays the pot to the best remaining hand, splitting ties. With one player left
    // that is simply the pot going to them. The whole pot goes to the winners, with no
    // side pots.
    void showdown() {
        uint32_t handMax = 0;
        int handMaxCount = 0;
        for (int i = 0; i < PLAYER_COUNT; i++) {
            if (players[i].inHand) {
                players[i].handScore = handScoreTable[getHandIndex(players[i].holeCards)];
                if (players[i].handScore > handMax) {
                    handMax = players[i].handScore;
                    handMaxCount = 1;
                } else if (players[i].handScore == handMax) {
                    handMaxCount++;
                }
            }
        }
        for (int i = 0; i < PLAYER_COUNT; i++) {
            if (players[i].inHand && players[i].handScore == handMax) {
                players[i].chips += pot / handMaxCount;
            }
        }
    }

    // Every player's chips at the end of the hand.
    std::array<float, PLAYER_COUNT> getRewards() {
        std::array<float, PLAYER_COUNT> rewards;
        for (int i = 0; i < PLAYER_COUNT; i++) {
            rewards[i] = players[i].chips;
        }
        return rewards;
    }
};
