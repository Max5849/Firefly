#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"

// The metas stage to build (see stagePath): 0 the full cumulative classes, 2 the
// per-street BoardClusterer classes. Stage 1 has no classifier yet.
int STAGE = 2;

struct BoardClusterer {

    std::array<int, 14> cardRanks;
    std::array<int, 4> cardSuits;  
    std::array<int, 14> suitedCardRanks;

    std::array<int, 13> cardRankBuckets;
    std::array<int, 4> flushDraws;
    std::array<std::array<int, 14>, 14> straightDraws;
    std::array<int, 14> straightOneDraws;
    int highCardRank;
    int highPairRank;

    BoardClusterer() {
        cardRankBuckets = {0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3};
    }  

    uint32_t classifyBoard(const std::array<int, 5>& boardCards, int street) {
        std::array<int, 9> classification;
        int classIndex = 0;
        std::fill(cardRanks.begin(), cardRanks.end(), 0);
        std::fill(cardSuits.begin(), cardSuits.end(), 0);
        flushDraws = {0};
        int flushDrawCount = 0;
        for (int i = 0; i < 3; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
        }
        for (int i = 0; i < 4; i++) {
            if (cardSuits[i] == 2) {
                flushDraws[i]++;
                flushDrawCount++;
            } 
        }

        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        }
        straightDraws = {0};
        straightOneDraws = {0};
        int straightDrawCount = 0;
        int straightOneDrawCount = 0;
        bool full = false;
        for (int i = 0; i < 10; i++) {
            if (cardRanks[i] > 0) {
                if (full) {
                    continue;
                } else {
                    full = true;
                }
            } else if (full) {
                full = false;
                continue;
            } 
            std::array<int, 5> drawCards;
            int singleStraightState = 0; 
            for (int j = i; j < i + 5; j++) {
                if (cardRanks[j] == 0) {
                    drawCards[singleStraightState] = j;
                    singleStraightState++;
                }
            }
            if (singleStraightState == 3) {
                straightDraws[drawCards[0]][drawCards[1]]++;
                straightDraws[drawCards[0]][drawCards[2]]++;
                straightDraws[drawCards[1]][drawCards[2]]++;
            } else if (singleStraightState == 2) {
                straightDraws[drawCards[0]][drawCards[1]] = -5;
                straightOneDraws[drawCards[0]]++;
                straightOneDraws[drawCards[1]]++;
            } 
        }
        for (int i = 0; i < 14; i++) {
            for (int j = i + 1; j < 14; j++) {
                if (straightOneDraws[i] < 2 && straightOneDraws[j] < 2) {
                    if (straightDraws[i][j] + straightOneDraws[i] + straightOneDraws[j] > 1) {
                        straightDraws[i][j] = 1;
                        straightDrawCount++;
                    } else {
                        straightDraws[i][j] = 0;
                    }
                } else {
                    straightDraws[i][j] = 0;
                }
            }
        }
        for (int i = 0; i < 14; i++) {
            if (straightOneDraws[i] > 1) {
                straightOneDraws[i] = 1;
                straightOneDrawCount++;
            } else {
                straightOneDraws[i] = 0;
            }
        }

        int flushState = 0;
        int flushSuit = 0;
        for (int i = 3; i < street + 2; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
        }
        for (int i = 0; i < 4; i++) {
            if (cardSuits[i] > 2) {
                flushState = cardSuits[i] - 1;
                flushSuit = i;
            } 
        }
        if (flushState < 1) {
            if (flushDrawCount < 1 || street == 3) {
                flushState = 0;
            } else {
                flushState = 1;
            }
        } 
        classification[classIndex++] = flushState;

        int suitedStraightState = 0;
        if (flushState > 2) {
            int suitedStraightOneFillCount = 0;
            int suitedStraightZeroFillCount = 0;
            std::fill(suitedCardRanks.begin(), suitedCardRanks.end(), 0);
            for (int i = 0; i < street + 2; i++) {
                if (boardCards[i] % 4 == flushSuit) {
                    suitedCardRanks[boardCards[i] / 4]++;
                }
            }
            if (suitedCardRanks[0] > 0) {
                suitedCardRanks[13] = 1;
            }
            bool full = false;
            for (int k = 0; k < 10; k++) {
                if (suitedCardRanks[k] > 0) {
                    if (full) {
                        continue;
                    } else {
                        full = true;
                    }
                } else if (full) {
                    full = false;
                    continue;
                } 
                int singleSuitedStraightState = 0; 
                for (int l = k; l < k + 5; l++) {
                    if (suitedCardRanks[l] == 0) {
                        singleSuitedStraightState++;
                    }
                }
                if (singleSuitedStraightState == 1) {
                    suitedStraightOneFillCount++;
                } else if (singleSuitedStraightState == 0) {
                    suitedStraightZeroFillCount++;
                }
            }
            if (suitedStraightOneFillCount < 1) {
                suitedStraightState = 0;
            } else if (suitedStraightOneFillCount < 2) {
                suitedStraightState = 1;
            } else {
                suitedStraightState = 2;
            }
        }
        classification[classIndex++] = suitedStraightState;

        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        }
        int straightState = 0;
        std::array<std::array<int, 14>, 14> straightFills = {0};
        std::array<int, 14> straightOneFills = {0};
        int straightFillCount = 0;
        int straightOneFillCount = 0;
        int straightZeroFillCount = 0;
        full = false;
        for (int i = 0; i < 10; i++) {
            if (cardRanks[i] > 0) {
                if (full) {
                    continue;
                } else {
                    full = true;
                }
            } else if (full) {
                full = false;
                continue;
            } 
            std::array<int, 5> drawCards;
            int singleStraightState = 0; 
            for (int j = i; j < i + 5; j++) {
                if (cardRanks[j] == 0) {
                    drawCards[singleStraightState] = j;
                    singleStraightState++;
                }
            }
            if (singleStraightState == 2) {
                straightFills[drawCards[0]][drawCards[1]]++;
                straightFillCount++;
            } else if (singleStraightState == 1) {
                straightOneFills[drawCards[0]]++;
                straightOneFillCount++;
            } else if (singleStraightState == 0) {
                straightZeroFillCount++;
            }
        }
        for (int i = 0; i < 14; i++) {
            for (int j = i + 1; j < 14; j++) {
                if (straightDraws[i][j] == 1 && straightFills[i][j] > 0) {
                    straightDraws[i][j] = 0;
                    straightDrawCount--;
                }
            }
        }

        if (classification[0] < 2) {
            if (straightZeroFillCount < 1) {
                if (straightOneFillCount < 1) {
                    if (straightFillCount < 1) {
                        if (straightDrawCount < 2 || street == 3) {
                            straightState = 0;
                        } else {
                            straightState = 1;
                        }
                    } else if (straightFillCount < 2) {
                        if (straightDrawCount < 2 || street == 3) {
                            straightState = 2;
                        } else {
                            straightState = 3;
                        }
                    } else if (straightFillCount < 3) {
                        straightState = 4;
                    } else {
                        straightState = 5;
                    }
                } else {
                    if (straightOneFillCount < 2) {
                        straightState = 6;
                    } else {
                        straightState = 7;
                    }
                } 
            } else {
                straightState = 8;
            }
        } else if (classification[0] == 2) {
            if (straightZeroFillCount < 1) {
                if (straightOneFillCount < 1) {
                    if (straightFillCount < 2) {
                        straightState = 0;
                    } else if (straightFillCount < 3) {
                        straightState = 4;
                    } else {
                        straightState = 5;
                    }
                } else {
                    if (straightOneFillCount < 2) {
                        straightState = 6;
                    } else {
                        straightState = 7;
                    }
                } 
            } else {
                straightState = 8;
            }
        } else if (classification[0] == 3) {
            if (straightZeroFillCount < 1) {
                if (straightOneFillCount < 1) {
                    straightState = 0;
                } else {
                    if (straightOneFillCount < 2) {
                        straightState = 6;
                    } else {
                        straightState = 7;
                    }
                } 
            } else {
                straightState = 8;
            }
        } else if (classification[0] == 4) {
            if (straightZeroFillCount < 1) {
                straightState = 0;
            } else {
                straightState = 8;
            }
        }
        
        classification[classIndex++] = straightState;

        highPairRank = 13;
        int highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
                highPairRank = i;
                highCard = i;
                classification[classIndex++] = cardRankBuckets[i];
                classification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                classification[classIndex++] = 0;
                classification[classIndex++] = 0;
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1 && i != highCard) {
                classification[classIndex++] = cardRankBuckets[i];
                classification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                classification[classIndex++] = 0;
                classification[classIndex++] = 0;
            }
        }

        highCardRank = 13;
        highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] == 1) {
                highCardRank = i;
                highCard = i;
                classification[classIndex++] = cardRankBuckets[i];
                classification[5] = 0;
                break;
            } else if (i == 12) {
                classification[classIndex++] = 4;
            }
        }

        if (classification[4] == 0) {
            for (int i = 0; i < 13; i++) {
                if (cardRanks[i] == 1 && i != highCard) {
                    classification[classIndex++] = cardRankBuckets[i];
                    break;
                } else if (i == 12) {
                    classification[classIndex++] = 4;
                }
            }
        } else {
            classification[classIndex++] = 4;
        }

        if (classification[4] == 2 || classification[6] == 2) {
            if (classification[0] < 3) {
                classification[0] = 0;
            }
            if (classification[2] < 6) {
                classification[2] = 0;
            }
        }
        
        uint32_t key = 0;
        key = 4 * key + street;
        key = 5 * key + classification[0];
        key = 4 * key + classification[1];
        key = 9 * key + classification[2];
        key = 4 * key + classification[3];
        key = 4 * key + classification[4];
        key = 4 * key + classification[5];
        key = 4 * key + classification[6];
        key = 5 * key + classification[7];
        key = 5 * key + classification[8];
        return key + 1;
    }    
};

int main() {
    if (STAGE != 0 && STAGE != 2) {
        std::cout << "no board classifier for stage " << STAGE << "\n";
        return 1;
    }
    Dealer dealer;
    HandClusterer handClusterer;
    BoardClusterer boardClusterer;
    std::vector<uint16_t> boardClasses(60929700);
    std::unordered_map<uint64_t, int> boardClassIndexes;
    int boardClassIndex = 0;
    // Gives each distinct key the next sequential id.
    auto assignClass = [&](uint64_t boardClassKey, int street) {
        auto [it, inserted] = boardClassIndexes.try_emplace(boardClassKey, boardClassIndex);
        if (inserted) {
            boardClassIndex++;
        }
        boardClasses[dealer.getBoardIndex(street)] = it->second;
    };

    // Full classes are cumulative: each street's key carries a history of the earlier
    // streets (see HandClusterer::flopHistory), and classifyTurn and classifyRiver read
    // state the earlier classifiers left. Stage 2 classes carry the street in their key,
    // so ids stay distinct across streets.
    while (dealer.flopGen()) {
        uint32_t flopClass = 0;
        if (STAGE == 0) {
            flopClass = handClusterer.classifyFlop(dealer.boardCards);
            assignClass(flopClass, 1);
        } else {
            assignClass(boardClusterer.classifyBoard(dealer.boardCards, 1), 1);
        }
        while (dealer.turnGen()) {
            uint32_t turnClass = 0;
            if (STAGE == 0) {
                handClusterer.classifyFlop(dealer.boardCards);
                turnClass = handClusterer.classifyTurn(dealer.boardCards);
                assignClass((uint64_t)handClusterer.flopHistory | ((uint64_t)turnClass << 18), 2);
            } else {
                assignClass(boardClusterer.classifyBoard(dealer.boardCards, 2), 2);
            }
            while (dealer.riverGen()) {
                if (STAGE == 0) {
                    handClusterer.classifyFlop(dealer.boardCards);
                    handClusterer.classifyTurn(dealer.boardCards);
                    uint32_t riverClass = handClusterer.classifyRiver(dealer.boardCards);
                    assignClass((uint64_t)handClusterer.flopHistory | ((uint64_t)handClusterer.turnHistory << 18)
                        | ((uint64_t)riverClass << 39), 3);
                } else {
                    assignClass(boardClusterer.classifyBoard(dealer.boardCards, 3), 3);
                }
                dealer.riverSet();
            }
            dealer.turnSet();
        }
        dealer.flopSet();
    }
    int maxClass = *std::max_element(boardClasses.begin(), boardClasses.end());
    std::cout << boardClassIndex << " board classes, largest id " << maxClass << "\n";
    // solver/trainer stores class + 169 in a 15-bit field.
    if (maxClass + 169 > 0x7FFF) {
        std::cout << "too many board classes for the 15-bit board field; nothing written\n";
        return 1;
    }
    std::string name = stagePath("boardClasses", STAGE);
    std::ofstream writeFile(name, std::ios::binary);
    writeFile.write(reinterpret_cast<char*>(boardClasses.data()), boardClasses.size() * sizeof(uint16_t));
    writeFile.close();
}
