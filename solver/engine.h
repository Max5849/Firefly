#pragma once

#include "solver/config.h"
#include "solver/handAnalysis/handClusterer.h"

// The two cards of every combo in holePairIndex order, and the 169 combo index
// deal() would compute for them. Built once.
struct ComboTables {
    std::array<std::array<int, 2>, COMBO_COUNT> cards;
    std::array<uint8_t, COMBO_COUNT> index169;

    ComboTables() {
        for (int first = 0; first < 52; first++) {
            for (int second = first + 1; second < 52; second++) {
                int combo = holePairIndex(first, second);
                cards[combo] = {first, second};
                int rank0 = first / 4;
                int rank1 = second / 4;
                index169[combo] = uint8_t(rank0 * (25 - rank0) / 2 + rank1
                    + (90 - rank0) * (first % 4 == second % 4));
            }
        }
    }
};

inline const ComboTables comboTables;

struct Engine {
    struct Player {
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
        std::array<int, 2> holeCards;
        int handScore;
        uint8_t handMetaBasic;
        uint8_t handMeta;
        // 5 bits
        float bet;
        std::array<uint8_t, 4> lastActions;
        // 4 * 1 bits
        uint8_t raisesFaced;
        // 2 bits
        uint8_t actionPosition;
        // 2 bits
        uint8_t relativePosition;
        // 1 bit
        uint8_t calls;
        // 1 bit
        // Range mode only: the reach of every combo, the product of the
        // probabilities this seat's combos had of taking each action it took.
        std::array<float, COMBO_COUNT> range;

        void set(std::array<int, 2>& holeCards, int handMeta, int position) {
            chips = CHIP_COUNT;
            this->position = position;
            positionBasic = position > 2 ? (3 + (position - 3) / 2) : position;
            villanPosition = 0;
            inHand = true;
            this->holeCards = holeCards;
            this->handMetaBasic = handMeta;
            this->handMeta = handMeta;
            handScore = 0;
            bet = 0;
            for (int i = 0; i < 4; i++) {
                lastActions[i] = 0;
            }
            raisesFaced = 0;
            actionPosition = 0;
            relativePosition = 0;
            calls = 0;
        }
    };

    std::array<std::array<uint32_t, 8>, 53> binomials;
    inline static std::vector<uint32_t> handScoreTable;
    inline static std::vector<uint16_t> boardClassTable;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetasBasic;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetas;
    HandClusterer handClusterer;
    std::array<Player, PLAYER_COUNT> players;
    std::array<int, 52> deck;
    std::array<int, 5> boardCards;
    uint8_t round;
    int heads;
    uint8_t roundHeadsBasic;
    // 2 bits
    uint8_t headsBasic;
    // 2 bits
    int firstAction;
    uint8_t action;
    uint8_t actionPosition;
    float bet;
    float pot;
    std::array<uint16_t, 4> boardClasses;
    // 1 * 15 bits
    std::array<uint8_t, 4> raiseHistory;
    uint8_t raiseHistoryCompressed;
    // 8 bits
    uint8_t raiseSize;
    // 1 bit
    uint8_t raiseStrength;
    // 2 bits
    uint8_t hiddenRaiseSize;
    // 1 bit
    uint8_t hiddenRaiseStrength;
    // 2 bits
    uint8_t calls;
    // 2 bits
    // Range mode: players hold ranges rather than hole cards. Set by dealRanges(),
    // cleared by deal().
    bool rangeMode = false;
    // Range mode: each combo's hand metas on the current street, filled by
    // newRound(). Undefined for combos that clash with the board.
    std::array<uint8_t, COMBO_COUNT> comboMetasBasic;
    std::array<uint8_t, COMBO_COUNT> comboMetas;
    // Range mode showdown scratch, allocated on first use.
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
        Engine::loadTables();
        std::iota(deck.begin(), deck.end(), 0);
    }

    static void loadTables(bool withHandScores = true) {
        static std::once_flag loadFlag;
        std::call_once(loadFlag, [withHandScores] {
            if (withHandScores) {
                const std::string handScorePath = "solver/handAnalysis/handScores.bin";
                handScoreTable.resize(133784560);
                uint64_t handScoreBytes = handScoreTable.size() * sizeof(uint32_t);
                if (fileSize(handScorePath) != handScoreBytes) {
                    throw std::runtime_error(handScorePath + " has an unexpected size");
                }
                std::ifstream handScoreFile = openRead(handScorePath);
                readExact(handScoreFile, handScoreTable.data(), handScoreBytes, handScorePath);
            }

            const std::string boardClassPath = "solver/handAnalysis/boardClasses.bin";
            boardClassTable.resize(60929700);
            uint64_t boardClassBytes = boardClassTable.size() * sizeof(uint16_t);
            if (fileSize(boardClassPath) != boardClassBytes) {
                throw std::runtime_error(boardClassPath + " has an unexpected size");
            }
            std::ifstream boardClassFile = openRead(boardClassPath);
            readExact(boardClassFile, boardClassTable.data(), boardClassBytes, boardClassPath);

            uint32_t maxBoardClass = *std::max_element(boardClassTable.begin(), boardClassTable.end());
            if (maxBoardClass + 169 + 3 >= 0x7FFF) {
                throw std::runtime_error("board class overflows the 15-bit key field");
            }

            loadHandMetas("solver/handAnalysis/handMetasBasic.bin", handMetasBasic);
            loadHandMetas("solver/handAnalysis/handMetas.bin", handMetas);
        });
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

    std::uniform_int_distribution<int> cardDist;

    void shuffle(std::mt19937& gen) {
        using param = std::uniform_int_distribution<int>::param_type;
        for (int i = 0; i < 2 * PLAYER_COUNT + 5; i++) {
            std::swap(deck[i], deck[cardDist(gen, param(i, 51))]);
        }
    }

    void deal() {
        dealBoard();
        for (int i = 0; i < PLAYER_COUNT; i++) {
            std::array<int, 2> holeCards = {deck[2 * i + 5], deck[2 * i + 6]};
            std::sort(holeCards.begin(), holeCards.end(), [](int a, int b) {
                return a < b;
            });
            int handMeta = (holeCards[0] / 4) * (25 - (holeCards[0] / 4)) / 2 + (holeCards[1] / 4)
                + (90 - (holeCards[0] / 4)) * (holeCards[0] % 4 == holeCards[1] % 4);
            players[i].set(holeCards, handMeta, i);
        }
        resetHand();
        rangeMode = false;
    }

    // Deals the board only. Every player holds a full range of 1326 combos at reach
    // 1.0 instead of hole cards; nothing is removed for the board until newRound()
    // reveals it.
    void dealRanges() {
        dealBoard();
        std::array<int, 2> noCards = {-1, -1};
        for (int i = 0; i < PLAYER_COUNT; i++) {
            players[i].set(noCards, 0, i);
            players[i].range.fill(1.0f);
        }
        resetHand();
        rangeMode = true;
    }

    void dealBoard() {
        boardCards = {deck[0], deck[1], deck[2], deck[3], deck[4]};
        std::sort(boardCards.begin(), boardCards.begin() + 3, [](int a, int b) {
            return a < b;
        });
    }

    void resetHand() {
        round = 0;
        heads = PLAYER_COUNT;
        roundHeadsBasic = heads > 3 ? 2 + (heads - 4) / 2 : heads - 2;
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
        for (int i = 0; i < 4; i++) {
            boardClasses[i] = 0;
            raiseHistory[i] = 0;
        }
        raiseHistoryCompressed = 0;
        raiseSize = 0;
        raiseStrength = 0;
        hiddenRaiseSize = 0;
        hiddenRaiseStrength = 0;
        calls = 0;
    }

    uint64_t getState(bool metasBasic, bool memoryBasic) {
        Player& player = players[action];
        Player& villan = players[player.villanPosition];
        uint64_t state = 0;
        int shift = 0;
        if (!memoryBasic) {
            state |= uint64_t(roundHeadsBasic & 0x3) << shift;
        }
        shift += 2;
        state |= uint64_t(headsBasic & 0x3) << shift;
        shift += 2;
        if (actionPosition < 1) {
            player.actionPosition = 0;
        } else if (actionPosition < 3) {
            player.actionPosition = 1;
        } else {
            player.actionPosition = 2;
        }    
        state |= uint64_t(player.actionPosition & 0x3) << shift;
        shift += 2;
        player.spr = float(player.chips - (bet - player.bet)) / (pot + (bet - player.bet));
        player.cpr = float(bet - player.bet) / (pot - (bet - player.bet));
        if (!memoryBasic) {
            if (player.spr < 0.01f) {
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
            if (player.spr < 0.01f) {
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
        if (!metasBasic) {
            if (round == 0) {
                state |= uint64_t(player.handMeta & 0x7FFF) << shift;
                state |= uint64_t(0 & 0x1F) << (shift + 15);
            } else {
                state |= uint64_t((boardClasses[round] + 169) & 0x7FFF) << shift;
                state |= uint64_t(player.handMeta & 0x1F) << (shift + 15);
            }
        } else {
            if (round == 0) {
                state |= uint64_t(player.handMetaBasic & 0x7FFF) << shift;
                state |= uint64_t(0 & 0x1F) << (shift + 15);
            } else {
                state |= uint64_t((boardClasses[1] + 169 + round) & 0x7FFF) << shift;
                state |= uint64_t(player.handMetaBasic & 0x1F) << (shift + 15);
            }
        }
        shift += 20;
        state |= uint64_t(raiseHistoryCompressed & 0xFF) << shift;
        shift += 8;
        for (int i = 0; i < 4; ++i) {
            state |= uint64_t(player.lastActions[i] & 0x1) << (shift + i);
        }
        shift += 4;
        for (int i = 0; i < 4; ++i) {
            state |= uint64_t(villan.lastActions[i] & 0x1) << (shift + i);
        }
        shift += 4;
        if (round < 1 || (!memoryBasic && round < 2)) {
            state |= uint64_t(player.positionBasic & 0x7) << shift;
        }
        shift += 3;
        if (round < 1 || (!memoryBasic && round < 2)) {
            state |= uint64_t(villan.positionBasic & 0x7) << shift;
        }
        shift += 3;
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
        return state;
    }

    std::array<bool, 5> getOptions() {
        std::array<bool, 5> options = {true, true, false, false, false};
        Player& player = players[action];
        if (player.cpr < 0.01f) {
            options[0] = false;
        } 
        int raises = raiseHistory[0] + raiseHistory[1] + raiseHistory[2] + raiseHistory[3];
        if (player.spr >= 0.01f && raises < 6) {
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
        if (players[action].chips < 0.01f) {
            return false;
        }
        roundHeadsBasic = heads > 3 ? 2 + (heads - 4) / 2 : heads - 2;
        actionPosition = heads - 1;
        bet = 0;
        boardClasses[round] = boardClassTable[getBoardIndex()];
        if (rangeMode) {
            revealRanges();
        }
        for (int i = 0; i < PLAYER_COUNT; i++) {
            if (players[i].inHand) {
                if (!rangeMode) {
                    uint32_t handClass = handClusterer.classifyHand(boardCards, players[i].holeCards, round);
                    uint64_t key = ((uint64_t)boardClasses[round]) | ((uint64_t)handClass << 32);
                    players[i].handMetaBasic = handMetasBasic.at(key);
                    players[i].handMeta = handMetas.at(key);
                }
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

    // Range mode, on entering a street: removes the combos holding a card this street
    // revealed from every range, then classifies each combo that survives the board
    // once, for all players, with the same meta lookup the hole card path makes.
    void revealRanges() {
        const int revealFrom = round == 1 ? 0 : round + 1;
        const int boardCount = round + 2;
        for (int combo = 0; combo < COMBO_COUNT; combo++) {
            const std::array<int, 2>& cards = comboTables.cards[combo];
            bool revealed = false;
            for (int i = revealFrom; i < boardCount; i++) {
                if (boardCards[i] == cards[0] || boardCards[i] == cards[1]) {
                    revealed = true;
                }
            }
            if (revealed) {
                for (int i = 0; i < PLAYER_COUNT; i++) {
                    players[i].range[combo] = 0.0f;
                }
                continue;
            }
            if (comboConflicts(combo, boardCount)) {
                continue;
            }
            uint32_t handClass = handClusterer.classifyHand(boardCards, cards, round);
            uint64_t key = ((uint64_t)boardClasses[round]) | ((uint64_t)handClass << 32);
            comboMetasBasic[combo] = handMetasBasic.at(key);
            comboMetas[combo] = handMetas.at(key);
        }
    }

    // Range mode: the acting player's state key if it held this combo. The combo
    // must not clash with the board revealed so far.
    uint64_t getComboState(int combo, bool metasBasic, bool memoryBasic) {
        Player& player = players[action];
        if (round == 0) {
            player.handMeta = comboTables.index169[combo];
            player.handMetaBasic = comboTables.index169[combo];
        } else {
            player.handMeta = comboMetas[combo];
            player.handMetaBasic = comboMetasBasic[combo];
        }
        player.holeCards = comboTables.cards[combo];
        return getState(metasBasic, memoryBasic);
    }

    // Range mode replacement for showdown() + getRewards(): every player's chips at
    // the end of the hand for each combo it might hold. Only a showdown makes this
    // depend on the combo.
    //
    // At a showdown each combo's share of the pot is its expectation against every
    // other live player's final range, excluding opponent combos that share a card
    // with it. Opponents are treated as independent of each other, so card removal
    // between opponents is ignored; ties split the pot exactly. Like showdown(), the
    // whole pot goes to the winners. Combos clashing with the full board are left at
    // the player's chips and must not be used.
    void getComboRewards(std::array<std::array<float, COMBO_COUNT>, PLAYER_COUNT>& out) {
        for (int i = 0; i < PLAYER_COUNT; i++) {
            out[i].fill(players[i].chips);
        }
        if (heads == 1) {
            for (int i = 0; i < PLAYER_COUNT; i++) {
                if (players[i].inHand) {
                    for (int combo = 0; combo < COMBO_COUNT; combo++) {
                        out[i][combo] += pot;
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
                out[i][combo] += float(pot * share);
            }
        }
    }

    void showdown() {
        int handMax = 0;
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
            if (players[i].inHand) {
                if (players[i].handScore == handMax) {
                    players[i].chips += pot / handMaxCount;
                } 
            }
        }
    }

    std::array<float, PLAYER_COUNT> getRewards() {
        std::array<float, PLAYER_COUNT> rewards;
        for (int i = 0; i < PLAYER_COUNT; i++) {
            rewards[i] = players[i].chips;
        }
        return rewards;
    }
};
