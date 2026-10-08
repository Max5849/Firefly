#include "solver/include/library.h"

struct HandClustererBasic {

    std::array<int, 14> cardRanks;
    std::array<int, 4> cardSuits;  
    std::array<int, 14> suitedCardRanks;

    std::array<int, 13> cardRankBuckets;
    int flushSuit;
    std::array<std::array<int, 13>, 13> straightDraws;
    std::array<int, 13> straightOneDraws;
    int highCardRank;
    int highPairRank;

    HandClustererBasic() {
        cardRankBuckets = {0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3};
    }  

    uint32_t classifyFlop(const std::array<int, 5>& boardCards) {
        std::array<int, 8> flopClassification;
        int classIndex = 0;
        std::fill(cardRanks.begin(), cardRanks.end(), 0);
        std::fill(cardSuits.begin(), cardSuits.end(), 0);
        flushSuit = -1;
        straightDraws = {0};
        straightOneDraws = {0};

        for (int i = 0; i < 3; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
            if (cardSuits[boardCards[i] % 4] == 2) {
                flushSuit = boardCards[i] % 4;
            }
        }
        if (flushSuit != -1) {
            flopClassification[classIndex++] = cardSuits[flushSuit] - 1;
        } else {
            flopClassification[classIndex++] = 0;
        }
    
        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        }
        int straightState = 0;
        std::array<std::array<int, 13>, 13> straightFills = {0};
        std::array<int, 13> straightOneFills = {0};
        int straightDrawCount = 0;
        int straightFillCount = 0;
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
                straightDraws[drawCards[0]][drawCards[2] % 13]++;
                straightDraws[drawCards[1]][drawCards[2] % 13]++;
            } else if (singleStraightState == 2) {
                straightFills[drawCards[0]][drawCards[1] % 13]++;
                straightFillCount++;
                straightOneDraws[drawCards[0]]++;
                straightOneDraws[drawCards[1] % 13]++;
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
        if (straightFillCount < 1) {
            if (straightDrawCount < 2) {
                straightState = 0;
            } else {
                straightState = 1;
            }
        } else {
            straightState = 4;
        }
        flopClassification[classIndex++] = straightState;
        highCardRank = 13;
        int highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] == 1) {
                highCardRank = i;
                highCard = i;
                flopClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                flopClassification[classIndex++] = 4;
            }
        }
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] == 1 && i != highCard) {
                flopClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                flopClassification[classIndex++] = 4;
            }
        }

        highPairRank = 13;
        highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
                highPairRank = i;
                highCard = i;
                flopClassification[classIndex++] = cardRankBuckets[i];
                flopClassification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                flopClassification[classIndex++] = 0;
                flopClassification[classIndex++] = 0;
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1 && i != highCard) {
                flopClassification[classIndex++] = cardRankBuckets[i];
                flopClassification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                flopClassification[classIndex++] = 0;
                flopClassification[classIndex++] = 0;
            }
        }
        uint32_t key = 0;
        key = 3 * key + flopClassification[0];
        key = 5 * key + flopClassification[1];
        if (flopClassification[5] == 0) {
            key = 5 * key + flopClassification[2];
        }
        // key = 5 * key + flopClassification[3];
        key = 4 * key + flopClassification[4];
        key = 4 * key + flopClassification[5];
        // key = 4 * key + flopClassification[6];
        key = 4 * key + flopClassification[7];
        return key + 1;
    }    

    uint32_t classifyTurn(const std::array<int, 5>& boardCards) {
        std::array<int, 10> turnClassification;
        int classIndex = 0;

        for (int i = 3; i < 4; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
        }
        if (flushSuit != -1) {
            turnClassification[classIndex++] = cardSuits[flushSuit] - 1;
        } else {
            turnClassification[classIndex++] = 0;
        }

        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        }
        int straightState = 0;
        std::array<std::array<int, 13>, 13> straightFills = {0};
        std::array<int, 13> straightOneFills = {0};
        int straightFillCount = 0;
        int straightCompleteCount = 0;
        int straightOneFillCount = 0;
        int straightOneCompleteCount = 0;
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
            if (singleStraightState == 2) {
                straightFills[drawCards[0]][drawCards[1] % 13]++;
                straightFillCount++;
            } else if (singleStraightState == 1) {
                straightOneFills[drawCards[0] % 13]++;
                straightOneFillCount++;
            } 
        }
        for (int i = 0; i < 13; i++) {
            for (int j = i + 1; j < 13; j++) {
                if (straightDraws[i][j] > 0 && straightFills[i][j] > 0) {
                    straightDraws[i][j] = 0;
                    straightCompleteCount++;
                } 
            }
        }
        for (int i = 0; i < 13; i++) {
            if (straightOneDraws[i] > 0 && straightOneFills[i] > 0) {
                straightOneDraws[i] = 0;
                straightOneCompleteCount++;
            }
        }
        if (straightOneFillCount < 1) {
            if (straightFillCount < 1) {
                straightState = 0;
            } else if (straightFillCount < 2) {
                straightState = 1;
            } else {
                straightState = 5;
            }
        } else {
            straightState = 9;
        }
        turnClassification[classIndex++] = straightState;

        int highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] == 1) {
                highCard = i;
                if (highCard < highCardRank) {
                    turnClassification[classIndex++] = 1;
                } else {
                    turnClassification[classIndex++] = 0;
                }
                highCardRank = highCard;
                turnClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                highCardRank = 13;
                turnClassification[classIndex++] = 0;
                turnClassification[classIndex++] = 4;
            }
        }
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] == 1 && i != highCard) {
                turnClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                turnClassification[classIndex++] = 4;
            }
        }

        highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
                highCard = i;
                if (highCard < highPairRank) {
                    highPairRank = highCard;
                    turnClassification[classIndex++] = 1;
                } else {
                    turnClassification[classIndex++] = 0;
                }
                turnClassification[classIndex++] = cardRankBuckets[i];
                turnClassification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                turnClassification[classIndex++] = 0;
                turnClassification[classIndex++] = 0;
                turnClassification[classIndex++] = 0;
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1 && i != highCard) {
                turnClassification[classIndex++] = cardRankBuckets[i];
                turnClassification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                turnClassification[classIndex++] = 0;
                turnClassification[classIndex++] = 0;
            }
        }

        uint32_t key = 0;
        key = 4 * key + turnClassification[0];
        key = 10 * key + turnClassification[1];
        // key = 2 * key + turnClassification[2];
        if (turnClassification[7] == 0) {
            key = 5 * key + turnClassification[3];
        }
        // key = 5 * key + turnClassification[4];
        // key = 2 * key + turnClassification[5];
        key = 4 * key + turnClassification[6];
        key = 4 * key + turnClassification[7];
        // key = 4 * key + turnClassification[8];
        key = 4 * key + turnClassification[9];
        return key + 1;
    }     

    uint32_t classifyRiver(const std::array<int, 5>& boardCards) {
        std::array<int, 11> riverClassification;
        int classIndex = 0;

        bool backdoorFlush = false;
        for (int i = 4; i < 5; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
            if (cardSuits[boardCards[i] % 4] == 3 && flushSuit != boardCards[i] % 4) {
                flushSuit = boardCards[i] % 4;
                backdoorFlush = true;
            }
        }
        if (flushSuit != -1) {
            if (!backdoorFlush) {
                riverClassification[classIndex++] = cardSuits[flushSuit] - 1;
            } else {
                riverClassification[classIndex++] = cardSuits[flushSuit] - 1;
            }
        } else {
            riverClassification[classIndex++] = 0;
        }
        int suitedStraightState = 0;
        int suitedStraightOneFillCount = 0;
        int suitedStraightZeroFillCount = 0;
        std::fill(suitedCardRanks.begin(), suitedCardRanks.end(), 0);
        for (int i = 0; i < 5; i++) {
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
            } else {
                suitedStraightState = 2;
            }
        } else {
            suitedStraightState = 3;
        }
        riverClassification[classIndex++] = suitedStraightState;

        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        }
        int straightState = 0;
        std::array<std::array<int, 13>, 13> straightFills = {0};
        std::array<int, 13> straightOneFills = {0};
        int straightFillCount = 0;
        int straightCompleteCount = 0;
        int straightOneFillCount = 0;
        int straightOneCompleteCount = 0;
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
                straightFills[drawCards[0]][drawCards[1] % 13]++;
                straightFillCount++;
            } else if (singleStraightState == 1) {
                straightOneFills[drawCards[0] % 13]++;
                straightOneFillCount++;
            } else if (singleStraightState == 0) {
                straightZeroFillCount++;
            }
        }
        for (int i = 0; i < 13; i++) {
            for (int j = i + 1; j < 13; j++) {
                if (straightDraws[i][j] > 0 && straightFills[i][j] > 0) {
                    straightCompleteCount++;
                } 
            }
        }
        for (int i = 0; i < 13; i++) {
            if (straightOneDraws[i] > 0 && straightOneFills[i] > 0) {
                straightOneCompleteCount++;
            }
        }
        if (straightZeroFillCount < 1) {
            if (straightOneFillCount < 1) {
                if (straightFillCount < 1) {
                    straightState = 0;
                } else if (straightFillCount < 2) {
                    straightState = 1;
                } else {
                    straightState = 5;
                }
            } else {
                straightState = 9;
            } 
        } else {
            straightState = 11;
        }
        riverClassification[classIndex++] = straightState;
        
        int highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] == 1) {
                highCard = i;
                if (highCard < highCardRank) {
                    riverClassification[classIndex++] = 1;
                } else {
                    riverClassification[classIndex++] = 0;
                }
                highCardRank = highCard;
                riverClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                highCardRank = 13;
                riverClassification[classIndex++] = 0;
                riverClassification[classIndex++] = 4;
            }
        }
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] == 1 && i != highCard) {
                riverClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                riverClassification[classIndex++] = 4;
            }
        }

        highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
                highCard = i;
                if (highCard < highPairRank) {
                    highPairRank = highCard;
                    riverClassification[classIndex++] = 1;
                } else {
                    riverClassification[classIndex++] = 0;
                }
                riverClassification[classIndex++] = cardRankBuckets[i];
                riverClassification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                riverClassification[classIndex++] = 0;
                riverClassification[classIndex++] = 0;
                riverClassification[classIndex++] = 0;
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1 && i != highCard) {
                riverClassification[classIndex++] = cardRankBuckets[i];
                riverClassification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
                riverClassification[classIndex++] = 0;
                riverClassification[classIndex++] = 0;
            }
        }

        uint32_t key = 0;
        key = 6 * key + riverClassification[0];
        key = 4 * key + riverClassification[1];
        key = 12 * key + riverClassification[2];
        // key = 2 * key + riverClassification[3];
        if (riverClassification[8] == 0) {
            key = 5 * key + riverClassification[4];
        }
        // key = 5 * key + riverClassification[5];
        // key = 2 * key + riverClassification[6];
        key = 4 * key + riverClassification[7];
        key = 4 * key + riverClassification[8];
        // key = 4 * key + riverClassification[9];
        key = 4 * key + riverClassification[10];
        return key + 1;
    }   
};
