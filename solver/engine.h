#pragma once

#include "solver/config.h"
#include "solver/handAnalysis/handClusterer.h"

// The two cards of every combo in holePairIndex order. Built once.
struct ComboTables {
    std::array<std::array<int, 2>, COMBO_COUNT> cards;

    ComboTables() {
        for (int first = 0; first < 52; first++) {
            for (int second = first + 1; second < 52; second++) {
                cards[holePairIndex(first, second)] = {first, second};
            }
        }
    }
};

inline const ComboTables comboTables;

struct Engine {
    struct Player {
        std::array<float, 1326> range;
        std::array<float, 1326> reward;
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

        void set(int position) {
            std::fill(range.begin(), range.end(), 1.0f);
            reward = {0};
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
    std::array<bool, 1326> commonRange;
    uint64_t actionState;
    uint64_t actionStateBasic;
    std::array<uint64_t, 1326> metaState;
    std::array<uint64_t, 1326> metaStateBasic;
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
    // Showdown scratch, allocated on first use.
    std::vector<double> rangeBelow;
    std::vector<double> rangeEqual;
    std::vector<double> rangeTotal;

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
        for (int i = 0; i < 5; i++) {
            std::swap(deck[i], deck[cardDist(gen, param(i, 51))]);
        }
    }

    void deal() {
        for (int i = 0; i < PLAYER_COUNT; i++) {
            players[i].set(i);
        }
        boardCards = {deck[0], deck[1], deck[2], deck[3], deck[4]};
        std::sort(boardCards.begin(), boardCards.begin() + 3, [](int a, int b) {
            return a < b;
        });
        std::fill(commonRange.begin(), commonRange.end(), 1);
        for (int i = 0; i < 1326; i++) {
            metaStateBasic[i] = preflopHandClasses[i];
            metaState[i] = preflopHandClasses[i];
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
                heroVillanStory += 2 * player.lastActions[i] + !memoryBasic * villan.lastActions[i];
            } else {
                heroVillanStory += player.lastActions[i] + !memoryBasic * villan.lastActions[i];
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

    std::array<bool, 1326>& getCommonRange() {
        return commonRange;
    }

    std::array<float, 1326>& getRange(int action) {
        return players[action].range;
    }

    std::array<float, 1326>& getReward(int action) {
        return players[action].reward;
    }

    uint64_t getActionState(bool memoryBasic) {
        return !memoryBasic ? actionState : actionStateBasic;
    }

    std::array<uint64_t, 1326>& getMetaState(bool metasBasic) {
        return !metasBasic ? metaState : metaStateBasic;
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

    void setCommonRange() {
        for (int i = 0; i < 51; i++) {
            for (int j = i + 1; j < 52; j++) {
                for (int k = 0; k < round + 2; k++) {
                    if (i == deck[k] || j == deck[k]) {
                        commonRange[i * (101 - i) / 2 + j - 1] = 0;
                        break;
                    } 
                }
            }
        }
    }

    // Classifies each combo once and fills whichever meta states are needed. Combos out
    // of every live range are skipped: reach only falls, so they are never looked up
    // again this hand.
    void setMetaState(bool needFull, bool needBasic) {
        uint32_t boardIndex = getBoardIndex();
        boardClass = boardClassTable[boardIndex];
        boardClassBasic = boardClassBasicTable[boardIndex];
        for (int i = 0; i < 51; i++) {
            for (int j = i + 1; j < 52; j++) {
                int index = i * (101 - i) / 2 + j - 1;
                if (!commonRange[index]) {
                    continue;
                }
                bool live = false;
                for (int p = 0; p < PLAYER_COUNT; p++) {
                    if (players[p].inHand && players[p].range[index] > 0.0f) {
                        live = true;
                        break;
                    }
                }
                if (!live) {
                    continue;
                }
                uint32_t handClass = handClusterer.classifyHand(boardCards, {i, j}, round);
                uint64_t key = uint64_t(boardClass) << 16 | uint64_t(handClass) << 32;
                if (needFull) {
                    metaState[index] = (uint64_t(boardClass + 169)
                        | uint64_t(handMetas.at(key)) << 16) & 0x3FFFFF;
                }
                if (needBasic) {
                    metaStateBasic[index] = (uint64_t(boardClassBasic + 169)
                        | uint64_t(handMetasBasic.at(key)) << 16) & 0x3FFFFF;
                }
            }
        }
    }

    // The combo's node within the current decision: combos with the same group share a
    // key, because the action state and the board part of the meta state are common to
    // the whole range. Preflop it is the 169 class, postflop the hand meta.
    uint8_t getGroup(bool metasBasic, int combo) {
        uint64_t meta = getMetaState(metasBasic)[combo];
        return uint8_t(round == 0 ? meta : meta >> 16);
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

    bool comboConflicts(int combo, int boardCount) const {
        const std::array<int, 2>& cards = comboTables.cards[combo];
        for (int i = 0; i < boardCount; i++) {
            if (boardCards[i] == cards[0] || boardCards[i] == cards[1]) {
                return true;
            }
        }
        return false;
    }

    // Every player's chips at the end of the hand for each combo it might hold. Only a
    // showdown makes this depend on the combo.
    //
    // At a showdown each combo's share of the pot is its expectation against every
    // other live player's final range, excluding opponent combos that share a card
    // with it. Opponents are treated as independent of each other, so card removal
    // between opponents is ignored; ties split the pot exactly. The whole pot goes to
    // the winners, with no side pots. Combos clashing with the full board are left at
    // the player's chips and must not be used.
    void setRewards() {
        for (int i = 0; i < PLAYER_COUNT; i++) {
            players[i].reward.fill(players[i].chips);
        }
        if (heads == 1) {
            for (int i = 0; i < PLAYER_COUNT; i++) {
                if (players[i].inHand) {
                    for (int combo = 0; combo < COMBO_COUNT; combo++) {
                        players[i].reward[combo] += pot;
                    }
                }
            }
            return;
        }

        std::array<int, COMBO_COUNT> order;
        std::array<uint32_t, COMBO_COUNT> scores;
        int count = 0;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            if (comboConflicts(combo, 5)) {
                continue;
            }
            std::array<int, 2> cards = comboTables.cards[combo];
            scores[combo] = handScoreTable[getHandIndex(cards)];
            order[count++] = combo;
        }
        std::sort(order.begin(), order.begin() + count, [&](int a, int b) {
            return scores[a] < scores[b];
        });

        rangeBelow.resize(PLAYER_COUNT * COMBO_COUNT);
        rangeEqual.resize(PLAYER_COUNT * COMBO_COUNT);
        rangeTotal.resize(PLAYER_COUNT * COMBO_COUNT);
        std::array<int, PLAYER_COUNT> live;
        int liveCount = 0;
        for (int j = 0; j < PLAYER_COUNT; j++) {
            if (!players[j].inHand) {
                continue;
            }
            live[liveCount++] = j;
            const std::array<float, COMBO_COUNT>& range = players[j].range;
            double* below = &rangeBelow[j * COMBO_COUNT];
            double* equal = &rangeEqual[j * COMBO_COUNT];
            double* total = &rangeTotal[j * COMBO_COUNT];

            double totalAll = 0;
            std::array<double, 52> totalCard = {};
            for (int k = 0; k < count; k++) {
                int combo = order[k];
                double r = range[combo];
                totalAll += r;
                totalCard[comboTables.cards[combo][0]] += r;
                totalCard[comboTables.cards[combo][1]] += r;
            }

            // One ascending pass. For combo c on cards a, b, the weight of combos
            // that do not share a card with c is W - W[a] - W[b] + w(c) by inclusion-
            // exclusion, and c is never below itself.
            double belowAll = 0;
            std::array<double, 52> belowCard = {};
            std::array<double, 52> equalCard = {};
            for (int start = 0; start < count; ) {
                int end = start;
                double equalAll = 0;
                while (end < count && scores[order[end]] == scores[order[start]]) {
                    int combo = order[end];
                    double r = range[combo];
                    equalAll += r;
                    equalCard[comboTables.cards[combo][0]] += r;
                    equalCard[comboTables.cards[combo][1]] += r;
                    end++;
                }
                for (int k = start; k < end; k++) {
                    int combo = order[k];
                    int a = comboTables.cards[combo][0];
                    int b = comboTables.cards[combo][1];
                    double r = range[combo];
                    below[combo] = std::max(0.0, belowAll - belowCard[a] - belowCard[b]);
                    equal[combo] = std::max(0.0, equalAll - equalCard[a] - equalCard[b] + r);
                    total[combo] = std::max(0.0, totalAll - totalCard[a] - totalCard[b] + r);
                }
                for (int k = start; k < end; k++) {
                    int combo = order[k];
                    int a = comboTables.cards[combo][0];
                    int b = comboTables.cards[combo][1];
                    double r = range[combo];
                    belowAll += r;
                    belowCard[a] += r;
                    belowCard[b] += r;
                    equalCard[a] = 0;
                    equalCard[b] = 0;
                }
                start = end;
            }
        }

        for (int x = 0; x < liveCount; x++) {
            int i = live[x];
            for (int k = 0; k < count; k++) {
                int combo = order[k];
                if (players[i].range[combo] <= 0.0f) {
                    continue;
                }
                // Coefficient m: every opponent at or below this combo, with exactly
                // m of them tied.
                std::array<double, PLAYER_COUNT> poly = {};
                poly[0] = 1.0;
                int degree = 0;
                for (int y = 0; y < liveCount; y++) {
                    int j = live[y];
                    double z = rangeTotal[j * COMBO_COUNT + combo];
                    if (j == i || z <= 0.0) {
                        continue;
                    }
                    double lo = rangeBelow[j * COMBO_COUNT + combo] / z;
                    double eq = rangeEqual[j * COMBO_COUNT + combo] / z;
                    degree++;
                    for (int m = degree; m > 0; m--) {
                        poly[m] = poly[m] * lo + poly[m - 1] * eq;
                    }
                    poly[0] *= lo;
                }
                double share = 0;
                for (int m = 0; m <= degree; m++) {
                    share += poly[m] / (m + 1);
                }
                players[i].reward[combo] += float(pot * share);
            }
        }
    }
};
