#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"

bool BASIC = false;

struct BoardClusterer {

    std::array<int, 14> cardRanks;
    std::array<int, 4> cardSuits;  
    std::array<int, 14> suitedCardRanks;

    std::array<int, 13> cardRankBuckets;
    int flushSuit;
    std::array<std::array<int, 13>, 13> straightDraws;
    std::array<int, 13> straightOneDraws;
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
        flushSuit = -1;
        straightDraws = {0};
        straightOneDraws = {0};

        for (int i = 0; i < street + 2; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
            if (flushSuit == -1) {
                if (street < 3) {
                    if (cardSuits[boardCards[i] % 4] == 2) {
                        flushSuit = boardCards[i] % 4;
                    }
                } else {
                    if (cardSuits[boardCards[i] % 4] == 3) {
                        flushSuit = boardCards[i] % 4;
                    }
                }
            }
        }
        if (flushSuit != -1) {
            classification[classIndex++] = cardSuits[flushSuit] - 1;
        } else {
            classification[classIndex++] = 0;
        }

        int suitedStraightState = 0;
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
        if (suitedStraightZeroFillCount < 1) { 
            if (suitedStraightOneFillCount < 1) {
                suitedStraightState = 0;
            } else if (suitedStraightOneFillCount < 2) {
                suitedStraightState = 1;
            } else {
                suitedStraightState = 2;
            }
        } else {
            suitedStraightState = 3;
        }
        classification[classIndex++] = suitedStraightState;

        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        }
        int straightState = 0;
        std::array<std::array<int, 13>, 13> straightFills = {0};
        std::array<int, 13> straightOneFills = {0};
        int straightDrawCount = 0;
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
            if (singleStraightState == 3) {
                straightDraws[drawCards[0]][drawCards[1]]++;
                straightDraws[drawCards[0]][drawCards[2] % 13]++;
                straightDraws[drawCards[1]][drawCards[2] % 13]++;
            } else if (singleStraightState == 2) {
                straightFills[drawCards[0]][drawCards[1] % 13]++;
                straightFillCount++;
                straightOneDraws[drawCards[0]]++;
                straightOneDraws[drawCards[1] % 13]++;
            } else if (singleStraightState == 1) {
                straightOneFills[drawCards[0] % 13]++;
                straightOneFillCount++;
            } else if (singleStraightState == 0) {
                straightZeroFillCount++;
            }
        }
        for (int i = 0; i < 13; i++) {
            for (int j = i + 1; j < 13; j++) {
                if (straightOneDraws[i] < 2 && straightOneDraws[j] < 2 && straightFills[i][j] < 1) {
                    if (straightDraws[i][j] + straightOneDraws[i] + straightOneDraws[j] > 1) {
                        straightDraws[i][j] = 1;
                        straightDrawCount++;
                    } 
                } else {
                    straightDraws[i][j] = 0;
                }
            }
        }
        for (int i = 0; i < 13; i++) {
            if (straightOneDraws[i] > 1) {
                straightOneDraws[i] = 1;
            } else {
                straightOneDraws[i] = 0;
            }
        }

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

    // Full classes are cumulative: each street's key carries the earlier streets' classes,
    // and classifyTurn and classifyRiver read state the earlier classifiers left. Basic
    // classes come from the current street's cards alone, in any order, and carry the
    // street themselves, so ids stay distinct across streets.
    while (dealer.flopGen()) {
        uint32_t flopClass = 0;
        if (!BASIC) {
            flopClass = handClusterer.classifyFlop(dealer.boardCards);
            assignClass(flopClass, 1);
        } else {
            assignClass(boardClusterer.classifyBoard(dealer.boardCards, 1), 1);
        }
        while (dealer.turnGen()) {
            uint32_t turnClass = 0;
            if (!BASIC) {
                handClusterer.classifyFlop(dealer.boardCards);
                turnClass = handClusterer.classifyTurn(dealer.boardCards);
                assignClass((uint64_t)flopClass | ((uint64_t)turnClass << 18), 2);
            } else {
                assignClass(boardClusterer.classifyBoard(dealer.boardCards, 2), 2);
            }
            while (dealer.riverGen()) {
                if (!BASIC) {
                    handClusterer.classifyFlop(dealer.boardCards);
                    handClusterer.classifyTurn(dealer.boardCards);
                    uint32_t riverClass = handClusterer.classifyRiver(dealer.boardCards);
                    assignClass((uint64_t)flopClass | ((uint64_t)turnClass << 18) | ((uint64_t)riverClass << 39), 3);
                } else {
                    assignClass(boardClusterer.classifyBoard(dealer.boardCards, 3), 3);
                }
                dealer.riverSet();
            }
            dealer.turnSet();
        }
        dealer.flopSet();
    }
    std::cout << boardClassIndex << " board classes\n";
    // The engine stores class + 169 in a 16-bit field.
    if (boardClassIndex - 1 + 169 > 0xFFFF) {
        std::cout << "too many board classes for the engine's 16-bit board field; nothing written\n";
        return 1;
    }
    std::string name = !BASIC ? "solver/handAnalysis/boardClasses.bin" : "solver/handAnalysis/boardClassesBasic.bin";
    std::ofstream writeFile(name, std::ios::binary);
    writeFile.write(reinterpret_cast<char*>(boardClasses.data()), boardClasses.size() * sizeof(uint16_t));
    writeFile.close();
}
