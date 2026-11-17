#pragma once

#include "solver/singleHand/config.h"
#include "solver/handAnalysis/handClusterer.h"

struct Engine {
    struct Player {
        std::array<int, 2> holeCards;
        // Indexed by metas stage.
        std::array<uint64_t, METAS_STAGE_COUNT> metaState;
        uint32_t handScore;
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
            metaState.fill(preflopHandClass);
            handScore = 0;
            chips = CHIP_COUNT;
            inHand = true;
            this->position = position;
            villanPosition = 2;
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
    // Indexed by metas stage.
    inline static std::array<std::vector<uint16_t>, METAS_STAGE_COUNT> boardClassTables;
    inline static std::array<ankerl::unordered_dense::map<uint64_t, uint8_t>, METAS_STAGE_COUNT> handMetas;
    inline static std::array<uint64_t, 1326> preflopHandClasses;
    // The preflop cluster per 169-class index, from preflopMetas.bin; stage C keys on it.
    inline static std::array<uint8_t, 169> preflopClusters;
    HandClusterer handClusterer;
    std::array<Player, PLAYER_COUNT> players;
    std::array<int, 52> deck;
    std::array<int, 5> boardCards;
    // Indexed by memory stage.
    std::array<uint64_t, MEMORY_STAGE_COUNT> actionState;
    uint8_t round;
    int heads;
    uint8_t headsBasic;
    uint8_t preflopHeadsBasic;
    int firstAction;
    bool opened;
    uint8_t openedPosition;
    uint8_t openedLimpers;
    uint8_t action;
    uint8_t actionPosition;
    float bet;
    float pot;
    // Indexed by metas stage.
    std::array<uint16_t, METAS_STAGE_COUNT> boardClass;
    std::array<uint8_t, 4> raiseHistory;
    uint8_t raiseHistoryCompressed;
    std::array<uint8_t, 4> raiseSizeHistory;
    uint8_t raiseStrength;
    uint8_t raiseActionPosition;
    uint8_t hiddenRaiseSize;
    uint8_t hiddenRaiseStrength;
    uint8_t hiddenRaiseActionPosition;
    uint8_t calls;
    uint8_t hiddenCalls;

    // The seven cards' lexicographic rank, handScoreTable's index. The skipped cards
    // between q and c sum in closed form: sum_{j=q+1}^{c-1} C(51-j, m) = C(51-q, m+1) - C(52-c, m+1).
    static uint32_t handIndex(const std::array<int, 5>& board, const std::array<int, 2>& holeCards) {
        std::array<int, 7> handCards = {board[0], board[1], board[2],
            board[3], board[4], holeCards[0], holeCards[1]};
        std::sort(handCards.begin(), handCards.end());
        uint32_t handIndex = 0;
        int q = -1;
        for (int i = 0; i < 7; i++) {
            handIndex += binomials[51 - q][7 - i] - binomials[52 - handCards[i]][7 - i];
            q = handCards[i];
        }
        return handIndex;
    }

    // RUNOUTS boards for each street a showdown can come before the river on, for the
    // hand being played, and every live seat's score on them. A street's boards are
    // drawn, and a seat's scores read, on first use, then shared by every branch and
    // traversal of the hand. reset() starts a new hand.
    struct Runouts {
        static_assert(PLAYER_COUNT <= 8, "scored holds a seat per bit");
        static_assert(RUNOUTS >= 1, "a showdown needs at least one runout");
        std::mt19937 gen;
        // Indexed by the street the hand stopped after: 0 preflop, 1 flop, 2 turn.
        std::array<std::array<std::array<int, 5>, RUNOUTS>, 3> boards;
        std::array<std::array<std::array<uint32_t, PLAYER_COUNT>, RUNOUTS>, 3> scores;
        std::array<bool, 3> drawn = {false, false, false};
        // Bit p: seat p's scores on the street's boards are read.
        std::array<uint8_t, 3> scored = {0, 0, 0};

        void reset() {
            drawn.fill(false);
            scored.fill(0);
        }

        // Each board keeps the seen cards and fills the rest from the unseen board cards
        // and the undealt deck, so no seat's hole cards, folded or not, can land on it.
        void draw(const Engine& engine, int street) {
            using param = std::uniform_int_distribution<int>::param_type;
            std::uniform_int_distribution<int> pick;
            const int seen = street == 0 ? 0 : street + 2;
            std::array<int, 52 - 2 * PLAYER_COUNT> pool;
            int size = 0;
            for (int i = seen; i < 5; i++) {
                pool[size++] = engine.deck[i];
            }
            for (int i = 5 + 2 * PLAYER_COUNT; i < 52; i++) {
                pool[size++] = engine.deck[i];
            }
            for (std::array<int, 5>& board : boards[street]) {
                for (int i = 0; i < seen; i++) {
                    board[i] = engine.boardCards[i];
                }
                for (int i = seen; i < 5; i++) {
                    std::swap(pool[i - seen], pool[pick(gen, param(i - seen, size - 1))]);
                    board[i] = pool[i - seen];
                }
            }
            drawn[street] = true;
        }

        // Pays each live seat its share of the pot, averaged over the street's boards.
        void split(Engine& engine, int street) {
            if (!drawn[street]) {
                draw(engine, street);
            }
            for (int p = 0; p < PLAYER_COUNT; p++) {
                if (!engine.players[p].inHand || (scored[street] >> p & 1)) {
                    continue;
                }
                // Every index first, so the table's cache misses overlap.
                std::array<uint32_t, RUNOUTS> indexes;
                for (int k = 0; k < RUNOUTS; k++) {
                    indexes[k] = handIndex(boards[street][k], engine.players[p].holeCards);
                    __builtin_prefetch(&handScoreTable[indexes[k]]);
                }
                for (int k = 0; k < RUNOUTS; k++) {
                    scores[street][k][p] = handScoreTable[indexes[k]];
                }
                scored[street] |= uint8_t(1u << p);
            }
            std::array<double, PLAYER_COUNT> share = {0};
            for (int k = 0; k < RUNOUTS; k++) {
                uint32_t best = 0;
                int count = 0;
                for (int p = 0; p < PLAYER_COUNT; p++) {
                    if (engine.players[p].inHand) {
                        const uint32_t score = scores[street][k][p];
                        if (score > best) {
                            best = score;
                            count = 1;
                        } else if (score == best) {
                            count++;
                        }
                    }
                }
                for (int p = 0; p < PLAYER_COUNT; p++) {
                    if (engine.players[p].inHand && scores[street][k][p] == best) {
                        share[p] += 1.0 / count;
                    }
                }
            }
            for (int p = 0; p < PLAYER_COUNT; p++) {
                if (engine.players[p].inHand) {
                    engine.players[p].chips += float(double(engine.pot) * share[p] / RUNOUTS);
                }
            }
        }
    };
    // The worker's runouts; null pays every showdown on the dealt board.
    Runouts* runouts = nullptr;

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

            for (int stage = 0; stage < METAS_STAGE_COUNT; stage++) {
                std::vector<uint16_t>& table = boardClassTables[stage];
                loadTable(stagePath("boardClasses", stage), table);
                if (*std::max_element(table.begin(), table.end()) + 169 > BOARD_CLASS_MASK) {
                    throw std::runtime_error("board class + 169 overflows the 15-bit key field");
                }
                loadHandMetas(stagePath("handMetas", stage), handMetas[stage]);
                for (const auto& [key, meta] : handMetas[stage]) {
                    if (meta > HAND_META_MASK) {
                        throw std::runtime_error("a hand meta overflows the 6-bit key field;"
                            " rebuild the hand metas with at most 64 clusters per board class");
                    }
                }
            }

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
            {
                const std::string path = "solver/handAnalysis/preflopMetas.bin";
                if (fileSize(path) != preflopClusters.size()) {
                    throw std::runtime_error(path + " has an unexpected size");
                }
                std::ifstream file = openRead(path);
                readExact(file, preflopClusters.data(), preflopClusters.size(), path);
                for (uint8_t cluster : preflopClusters) {
                    // A preflop board field must stay below the postflop ones, which start at 169.
                    if (cluster >= 169) {
                        throw std::runtime_error(path + " has a cluster of 169 or more");
                    }
                }
            }

            // equityClusterer.cpp's preflopClass() copies this formula; keep the two identical.
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
            const uint64_t preflopClass = preflopHandClasses[holePairIndex(holeCards[0], holeCards[1])];
            players[i].set(holeCards, preflopClass, i);
            // Stage C keys preflop on the class's cluster, in the board field like a class.
            players[i].metaState[2] = preflopClusters[preflopClass];
        }
        round = 0;
        heads = PLAYER_COUNT;
        headsBasic = heads > 3 ? 2 + (heads - 4) / 2 : heads - 2;
        preflopHeadsBasic = headsBasic;
        firstAction = 3 % PLAYER_COUNT;
        opened = false;
        openedPosition = 0;
        openedLimpers = 0;
        action = firstAction;
        actionPosition = PLAYER_COUNT - 1;
        bet = 2;
        players[1].chips -= 1;
        players[1].bet = 1;
        players[2].chips -= 2;
        players[2].bet = 2;
        pot = 3 + ANTE;
        raiseHistory = {0};
        raiseHistoryCompressed = 0;
        raiseSizeHistory = {0};
        raiseStrength = 0;
        raiseActionPosition = 0;
        hiddenRaiseSize = 0;
        hiddenRaiseStrength = 0;
        hiddenRaiseActionPosition = 0;
        calls = 0;
        hiddenCalls = 0;
    }

    void setActionState(int memoryStage) {
        Player& player = players[action];
        Player& villan = players[player.villanPosition];
        uint64_t state = 0;
        int shift = 0;
        state |= uint64_t(headsBasic & 0x3) << shift;
        shift += 2;
        state |= uint64_t(preflopHeadsBasic & 0x3) << shift;
        shift += 2;

        player.spr = float(player.chips - (bet - player.bet)) / (pot + (bet - player.bet));
        player.cpr = float(bet - player.bet) / (pot - (bet - player.bet));
        if (player.spr < 0.0005f) {
            if (player.cpr < 0.5f) {
                state |= uint64_t(6 & 0xF) << shift;
            } else if (player.cpr < 1.5f) {
                state |= uint64_t(7 & 0xF) << shift;
            } else if (player.cpr < 3.5f) {
                state |= uint64_t(8 & 0xF) << shift;
            } else {
                state |= uint64_t(9 & 0xF) << shift;
            } 
        } else if (player.spr < 0.5f) {
            state |= uint64_t(0 & 0xF) << shift;
        } else if (player.spr < 1.5f) {
            state |= uint64_t(1 & 0xF) << shift;
        } else if (player.spr < 3.5f) {
            state |= uint64_t(2 & 0xF) << shift;
        } else if (player.spr < 7.5f) {
            state |= uint64_t(3 & 0xF) << shift;
        } else if (player.spr < 15.5f) {
            state |= uint64_t(4 & 0xF) << shift;
        } else {
            state |= uint64_t(5 & 0xF) << shift;
        }
        shift += 4;

        state |= uint64_t(raiseHistoryCompressed & 0xFF) << shift;
        shift += 8;
        for (int i = 0; i < 4; i++) {
            state |= uint64_t((raiseSizeHistory[i] > 0) & 0x1) << (shift + i);
        }
        shift += 4;

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
        heroVillanStory += (round == 0 || raiseHistory[0] > 0) * player.positionBasic;
        heroVillanStory *= 5;
        heroVillanStory += (round == 0 || raiseHistory[0] > 0) * villan.positionBasic;
        state |= uint64_t(heroVillanStory & 0x7FF) << shift;
        shift += 11;

        if (raiseHistory[round] > 0) {
            uint64_t pointState = 0;
            pointState += std::min(raiseStrength, uint8_t(2));
            pointState *= 3;
            pointState += (raiseActionPosition > 0) + (raiseActionPosition > 2);
            pointState *= 3;
            pointState += std::min(hiddenCalls, uint8_t(2));
            state |= uint64_t(pointState & 0x1F) << shift;
        }
        shift += 5;

        if (raiseHistory[round] > 1) {
            state |= uint64_t((hiddenRaiseSize > 0) & 0x1) << shift;
        }
        shift += 1;

        uint64_t pointState = 0;
        pointState += (actionPosition > 0) + (actionPosition > 2);
        pointState *= 3;
        pointState += std::min(player.raisesFaced, uint8_t(2));
        pointState *= 3;
        pointState += std::min(calls, uint8_t(2));
        state |= uint64_t(pointState & 0x1F) << shift;
        shift += 5;

        state |= uint64_t(player.relativePosition & 0x1) << shift;
        shift += 1;

        actionState[memoryStage] = state;
    }

    // A player who has already called twice this street folds to any further bet. The
    // fold is automatic: the caller plays it without building a key or a node.
    bool autoFold() const {
        const Player& player = players[action];
        const float toCall = bet - player.bet;
        return player.calls > 1 && toCall / (pot - toCall) >= 0.0005f;
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
                if (player.spr < 7.5f) {
                    options[4] = true;
                } 
                if (player.spr >= 3.5f) {
                    options[3] = true;
                }
                if (player.spr >= 1.5f) {
                    options[2] = true;
                }
            }
            if (player.raisesFaced > 2) {
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

    int getHoleCombo(int action) {
        return holePairIndex(players[action].holeCards[0], players[action].holeCards[1]);
    }

    uint64_t getActionState(int memoryStage) {
        return actionState[memoryStage];
    }

    uint64_t getMetaState(int metasStage) {
        return players[action].metaState[metasStage];
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
            if (round == 0) {
                preflopHeadsBasic = headsBasic;
            }
        } else if (option == 1) {
            player.chips -= (bet - player.bet);
            pot += (bet - player.bet);
            player.bet = bet;
            player.lastActions[round] = 0;
            player.raisesFaced = 0;
            if (opened) {
                if (calls == 0) {
                    players[firstAction].villanPosition = player.position;
                    if ((firstAction + PLAYER_COUNT - 1) % PLAYER_COUNT 
                        <= (action + PLAYER_COUNT - 1) % PLAYER_COUNT) {
                        players[firstAction].relativePosition = 0;
                    } else {
                        players[firstAction].relativePosition = 1;
                    }
                }
                calls++;
            } 
            if (round == 0 && !opened) {
                for (int i = 0; i < PLAYER_COUNT; i++) {
                    if (players[i].inHand && i != action) {
                        players[i].villanPosition = player.position;
                        if ((i + PLAYER_COUNT - 1) % PLAYER_COUNT 
                            <= (action + PLAYER_COUNT - 1) % PLAYER_COUNT) {
                            players[i].relativePosition = 0;
                        } else {
                            players[i].relativePosition = 1;
                        }
                    }
                }
                firstAction = action;
                opened = true;
            }
            player.calls++;
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
            hiddenRaiseSize = raiseSizeHistory[round];
            hiddenRaiseStrength = raiseStrength;
            raiseSizeHistory[round] = option - 2;
            raiseStrength = player.raisesFaced;
            player.raisesFaced = 0;
            for (int i = 0; i < PLAYER_COUNT; i++) {
                if (players[i].inHand && i != action) {
                    players[i].raisesFaced++;
                    players[i].villanPosition = player.position;
                    if ((i + PLAYER_COUNT - 1) % PLAYER_COUNT 
                        <= (action + PLAYER_COUNT - 1) % PLAYER_COUNT) {
                        players[i].relativePosition = 0;
                    } else {
                        players[i].relativePosition = 1;
                    }
                }
            }
            if (round == 0 && !opened) {
                openedPosition = action;
                openedLimpers = calls;
            }
            opened = true;
            hiddenCalls = calls;
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

    // Fills the meta state of every metas stage whose bit is set in stageMask.
    void setMetaState(uint32_t stageMask) {
        uint32_t boardIndex = getBoardIndex();
        for (int stage = 0; stage < METAS_STAGE_COUNT; stage++) {
            boardClass[stage] = boardClassTables[stage][boardIndex];
        }
        for (int p = 0; p < PLAYER_COUNT; p++) {
            Player& player = players[p];
            if (!player.inHand) {
                continue;
            }
            uint32_t handClass = handClusterer.classifyHand(boardCards, player.holeCards, round);
            for (int stage = 0; stage < METAS_STAGE_COUNT; stage++) {
                if (stageMask >> stage & 1) {
                    uint64_t key = uint64_t(boardClass[stage]) | uint64_t(handClass) << 16;
                    player.metaState[stage] = uint64_t(boardClass[stage] + 169)
                        | uint64_t(handMetas[stage].at(key)) << BOARD_CLASS_BITS;
                }
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
        if (heads > 3 && (raiseHistory[round - 1] > 0)) {
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
        opened = false;
        calls = 0;
        return true;
    }

    void showdown() {
        if (heads == 1) {
            for (int i = 0; i < PLAYER_COUNT; i++) {
                if (players[i].inHand) {
                    players[i].chips += pot;
                }
            }
            return;
        }
        if (round < 4 && runouts != nullptr) {
            runouts->split(*this, round - 1);
            return;
        }
        uint32_t handMax = 0;
        int handMaxCount = 0;
        for (int i = 0; i < PLAYER_COUNT; i++) {
            if (players[i].inHand) {
                players[i].handScore = handScoreTable[handIndex(boardCards, players[i].holeCards)];
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

    std::array<float, PLAYER_COUNT> getRewards() {
        std::array<float, PLAYER_COUNT> rewards;
        for (int i = 0; i < PLAYER_COUNT; i++) {
            rewards[i] = players[i].chips;
        }
        return rewards;
    }
};
