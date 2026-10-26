#include "solver/include/library.h"

struct HandClusterer {

    std::array<int, 14> cardRanks;
    std::array<int, 4> cardSuits;  
    std::array<int, 14> holeCardRanks;
    std::array<int, 4> holeCardSuits;
    std::array<int, 14> suitedCardRanks;
    std::array<int, 14> suitedHoleCardRanks;

    std::array<int, 13> cardRankBuckets;
    std::array<int, 13> holeCardRankBuckets;
    int flushSuit;
    std::array<std::array<int, 13>, 13> straightDraws;
    std::array<int, 13> straightOneDraws;
    int highCardRank;
    int highPairRank;

    HandClusterer() {
        cardRankBuckets = {0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3};
        holeCardRankBuckets = {0, 1, 2, 3, 3, 4, 4, 5, 5, 5, 6, 6, 6};
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
        } else if (straightFillCount < 2) {
            if (straightDrawCount < 2) {
                straightState = 2;
            } else {
                straightState = 3;
            }
        } else if (straightFillCount < 3) {
            straightState = 4;
        } else {
            straightState = 5;
        }
        flopClassification[classIndex++] = straightState;

        highPairRank = 13;
        int highCard = 0;
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

        highCardRank = 13;
        highCard = 0;
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
        if (flopClassification[3] == 0) {
            for (int i = 0; i < 13; i++) {
                if (cardRanks[i] == 1 && i != highCard) {
                    flopClassification[classIndex++] = cardRankBuckets[i];
                    break;
                } else if (i == 12) {
                    flopClassification[classIndex++] = 4;
                }
            }
        } else {
            flopClassification[classIndex++] = 4;
        }

        uint32_t key = 0;
        key = 3 * key + flopClassification[0];
        key = 6 * key + flopClassification[1];
        key = 4 * key + flopClassification[2];
        key = 4 * key + flopClassification[3];
        key = 4 * key + flopClassification[4];
        key = 4 * key + flopClassification[5];
        key = 5 * key + flopClassification[6];
        key = 5 * key + flopClassification[7];
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
                if (straightCompleteCount < 1) {
                    straightState = 1;
                } else {
                    straightState = 2;
                }
            } else if (straightFillCount < 3) {
                if (straightCompleteCount < 1) {
                    straightState = 3;
                } else {
                    straightState = 4;
                }
            } else {
                straightState = 5;
            }
        } else {
            if (straightOneFillCount < 2) {
                if (straightOneCompleteCount < 1) {
                    straightState = 6;
                } else {
                    straightState = 7;
                }
            } else {
                if (straightOneCompleteCount < 1) {
                    straightState = 8;
                } else {
                    straightState = 9;
                }
            }
        }
        turnClassification[classIndex++] = straightState;

        int highCard = 0;
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

        highCard = 0;
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
        if (turnClassification[4] == 0) {
            for (int i = 0; i < 13; i++) {
                if (cardRanks[i] == 1 && i != highCard) {
                    turnClassification[classIndex++] = cardRankBuckets[i];
                    break;
                } else if (i == 12) {
                    turnClassification[classIndex++] = 4;
                }
            }
        } else {
            turnClassification[classIndex++] = 4;
        }

        uint32_t key = 0;
        key = 4 * key + turnClassification[0];
        key = 10 * key + turnClassification[1];
        key = 2 * key + turnClassification[2];
        key = 4 * key + turnClassification[3];
        key = 4 * key + turnClassification[4];
        key = 4 * key + turnClassification[5];
        key = 4 * key + turnClassification[6];
        key = 2 * key + turnClassification[7];
        key = 5 * key + turnClassification[8];
        key = 5 * key + turnClassification[9];
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
                riverClassification[classIndex++] = 5;
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
            } else if (suitedStraightOneFillCount < 2) {
                suitedStraightState = 1;
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
                    if (straightCompleteCount < 1) {
                        straightState = 1;
                    } else {
                        straightState = 2;
                    }
                } else if (straightFillCount < 3) {
                    if (straightCompleteCount < 1) {
                        straightState = 3;
                    } else {
                        straightState = 4;
                    }
                } else {
                    straightState = 5;
                }
            } else {
                if (straightOneFillCount < 2) {
                    if (straightOneCompleteCount < 1) {
                        straightState = 6;
                    } else {
                        straightState = 7;
                    }
                } else {
                    if (straightOneCompleteCount < 1) {
                        straightState = 8;
                    } else {
                        straightState = 9;
                    }
                }
            } 
        } else {
            if (straightOneCompleteCount < 1) {
                straightState = 10;
            } else {
                straightState = 11;
            }
        }
        riverClassification[classIndex++] = straightState;
        
        int highCard = 0;
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

        highCard = 0;
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
        if (riverClassification[5] == 0) {
            for (int i = 0; i < 13; i++) {
                if (cardRanks[i] == 1 && i != highCard) {
                    riverClassification[classIndex++] = cardRankBuckets[i];
                    break;
                } else if (i == 12) {
                    riverClassification[classIndex++] = 4;
                }
            }
        } else {
            riverClassification[classIndex++] = 4;
        }
        
        uint32_t key = 0;
        key = 6 * key + riverClassification[0];
        key = 4 * key + riverClassification[1];
        key = 12 * key + riverClassification[2];
        key = 2 * key + riverClassification[3];
        key = 4 * key + riverClassification[4];
        key = 4 * key + riverClassification[5];
        key = 4 * key + riverClassification[6];
        key = 4 * key + riverClassification[7];
        key = 2 * key + riverClassification[8];
        key = 5 * key + riverClassification[9];
        key = 5 * key + riverClassification[10];
        return key + 1;
    }   

    uint32_t classifyHand(const std::array<int, 5>& boardCards, const std::array<int, 2>& holeCards, int street) {
        std::array<int, 14> handClassification;
        int classIndex = 0;

        std::fill(cardRanks.begin(), cardRanks.end(), 0);
        std::fill(cardSuits.begin(), cardSuits.end(), 0);
        for (int i = 0; i < street + 2; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
        }
        std::fill(holeCardRanks.begin(), holeCardRanks.end(), 0);
        std::fill(holeCardSuits.begin(), holeCardSuits.end(), 0);
        for (int i = 0; i < 2; i++) {
            holeCardRanks[holeCards[i] / 4]++;
            holeCardSuits[holeCards[i] % 4]++;
        }

        int flushState = 0;
        int holeFlushSuit = -1;
        for (int i = 0; i < 4; i++) {
            if (holeCardSuits[i] > 0 && cardSuits[i] + holeCardSuits[i] > street + 1) {
                flushState = std::min(cardSuits[i] + holeCardSuits[i] - 2, 3);
                holeFlushSuit = i;
                break;
            }
        }

        int suitedStraightState = 0;
        int suitedStraightRank = 0;
        std::array<int, 2> suitedStraightStates = {0, 0};
        std::array<std::array<int, 2>, 2> suitedStraightRanks = {0, 0};
        if (flushState > 1) {
            std::fill(suitedCardRanks.begin(), suitedCardRanks.end(), 0);
            for (int j = 0; j < street + 2; j++) {
                if (boardCards[j] % 4 == holeFlushSuit) {
                    suitedCardRanks[boardCards[j] / 4]++;
                }
            }
            if (suitedCardRanks[0] > 0) {   
                suitedCardRanks[13] = 1;
            }
            std::fill(suitedHoleCardRanks.begin(), suitedHoleCardRanks.end(), 0);
            for (int j = 0; j < 2; j++) {
                if (holeCards[j] % 4 == holeFlushSuit) {
                    suitedHoleCardRanks[holeCards[j] / 4]++;
                }
            }
            if (suitedHoleCardRanks[0] > 0) {
                suitedHoleCardRanks[13] = 1;
            }
            bool full = false;
            for (int i = 0; i < 10; i++) {
                if (suitedCardRanks[i] + suitedHoleCardRanks[i] > 0) {
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
                int holeCardCount = 0; 
                for (int j = i; j < i + 5; j++) {
                    if (suitedCardRanks[j] + suitedHoleCardRanks[j] == 0) {
                        singleSuitedStraightState++; 
                    } else if (suitedCardRanks[j] == 0) {
                        holeCardCount++;
                    }
                }
                if (holeCardCount > 0 && singleSuitedStraightState < 2 && singleSuitedStraightState < 4 - street) {
                    int index = 1 - singleSuitedStraightState;
                    if (suitedStraightStates[index] < 2) {
                        bool fullRank = false;
                        for (int k = std::max(i - 4, 0); k < i; k++) {
                            if (suitedCardRanks[k] > 0 || (k >= i && suitedHoleCardRanks[k] == 0)) {
                                if (fullRank) {
                                    continue;
                                } else {
                                    fullRank = true;
                                }
                            } else if (fullRank) {
                                fullRank = false;
                                continue;
                            } 
                            int singleStraightRank = 0;
                            for (int l = k; l < k + 5; l++) {
                                if (suitedCardRanks[l] == 0 && !(l >= i && suitedHoleCardRanks[l] == 0)) {
                                    singleStraightRank++; 
                                }                     
                            }
                            if (singleStraightRank == 1) {
                                suitedStraightRanks[index][suitedStraightStates[index]] = 1;
                                break;
                            } 
                        }
                    }
                    suitedStraightStates[1 - singleSuitedStraightState]++;
                }
            }
        } 
        if (suitedStraightStates[1] > 0) {
            suitedStraightState = 3;
            suitedStraightRank = 2 * suitedStraightRanks[1][0] + suitedStraightRanks[1][1];
        } else if (suitedStraightStates[0] > 0) {
            if (suitedStraightStates[0] == 1) {
                suitedStraightState = 2;
                suitedStraightRank = 2 * suitedStraightRanks[0][0] + suitedStraightRanks[0][1];
            } else {
                suitedStraightState = 1;
                suitedStraightRank = 2 * suitedStraightRanks[0][0] + suitedStraightRanks[0][1];
            }
        } else {
            suitedStraightState = 0;
            suitedStraightRank = 0;
        }
        handClassification[classIndex++] = suitedStraightState;
        handClassification[classIndex++] = suitedStraightRank;

        int flushRank = 0;
        bool suited = 0;
        if (flushState > 0) {
            int highCard = 0;
            if (holeCards[0] % 4 == holeFlushSuit) {
                highCard = holeCards[0] / 4;
            } else if (holeCards[1] % 4 == holeFlushSuit) {
                highCard = holeCards[1] / 4;
            } 
            int overCount = 0;
            for (int q = 0; q < street + 2; q++) {
                if (boardCards[q] % 4 == holeFlushSuit && boardCards[q] / 4 < highCard) {
                    overCount++;
                }
            }
            if (overCount > 4 || (flushState == 1 && highCard - overCount > 4)) {
                flushState = 0;
            } else {
                suited = holeCards[0] % 4 == holeCards[1] % 4;
                if (highCard - overCount < 1) {
                    flushRank = 0;
                } else if (highCard - overCount < 2) {
                    flushRank = 1;
                } else if (highCard - overCount < 3) {
                    flushRank = 2;
                } else if (highCard - overCount < 5) {
                    flushRank = 3;
                } else if (highCard - overCount < 7) {
                    flushRank = 4;
                } else {
                    flushRank = 5;
                }
            }
        }
        handClassification[classIndex++] = flushState;
        handClassification[classIndex++] = flushRank;
        handClassification[classIndex++] = suited;

        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        }
        if (holeCardRanks[0] > 0) {
            holeCardRanks[13] = 1;
        }
        int straightState = 0;
        int straightRank = 0;
        std::array<int, 3> straightStates = {0};
        std::array<std::array<int, 2>, 3> straightRanks = {0};
        bool full = false;
        for (int i = 0; i < 10; i++) {
            if (holeCardRanks[i] + cardRanks[i] > 0) {
                if (full) {
                    continue;
                } else {
                    full = true;
                }
            } else if (full) {
                full = false;
                continue;
            } 
            int singleStraightState = 0;
            int holeCardCount = 0; 
            for (int j = i; j < i + 5; j++) {
                if (cardRanks[j] + holeCardRanks[j] == 0) {
                    singleStraightState++; 
                } else if (cardRanks[j] == 0) {
                    holeCardCount++;
                }
            }
            if (holeCardCount > 0 && singleStraightState < 4 - street) {
                int index = 2 - singleStraightState;
                if (straightStates[index] < 2) {
                    bool fullRank = false;
                    for (int k = std::max(i - 4, 0); k < i; k++) {
                        if (cardRanks[k] > 0 || (k >= i && holeCardRanks[k] == 0)) {
                            if (fullRank) {
                                continue;
                            } else {
                                fullRank = true;
                            }
                        } else if (fullRank) {
                            fullRank = false;
                            continue;
                        } 
                        int singleStraightRank = 0;
                        for (int l = k; l < k + 5; l++) {
                            if (cardRanks[l] == 0 && !(l >= i && holeCardRanks[l] == 0)) {
                                singleStraightRank++; 
                            } 
                        }
                        if (singleStraightRank == 1) {
                            straightRanks[index][straightStates[index]] = 3;
                            break;
                        } else if (singleStraightRank == 2) {
                            if (straightRanks[index][straightStates[index]] == 0) {
                                straightRanks[index][straightStates[index]] = 2;
                            } else {
                                straightRanks[index][straightStates[index]] = 1;
                                break;
                            }
                        }
                    }
                }
                straightStates[index]++;
            }
        }
        if (straightStates[2] > 0) {
            straightState = 4;
            straightRank = 4 * straightRanks[2][0] + straightRanks[2][1];
        } else if (straightStates[1] > 0) {
            if (straightStates[1] == 1) {
                straightState = 3;
                straightRank = 4 * straightRanks[1][0] + straightRanks[1][1];
            } else {
                straightState = 2;
                straightRank = 4 * straightRanks[1][0] + straightRanks[1][1];
            }
        } else if (straightStates[0] > 1 && straightRanks[0][0] < 3 && straightRanks[0][1] < 3) {
            straightState = 1;
            straightRank = 4 * straightRanks[0][0] + straightRanks[0][1];
        } else {
            straightState = 0;
            straightRank = 0;
        }
        handClassification[classIndex++] = straightState;
        handClassification[classIndex++] = straightRank;
        
        int relativeRank = 0;
        for (int i = 0; i < 13; i++) {
            for (int j = 0; j < holeCardRanks[i]; j++) {
                handClassification[classIndex++] = holeCardRankBuckets[i];
                if (relativeRank < 1) {
                    handClassification[classIndex++] = 0;
                } else if (relativeRank < 2) {
                    handClassification[classIndex++] = 1;
                } else if (relativeRank < 4) {
                    handClassification[classIndex++] = 2;
                } else {
                    handClassification[classIndex++] = 3;
                }
                handClassification[classIndex++] = cardRanks[i] + holeCardRanks[i] - 1;
            }
            if (cardRanks[i] > 0) {
                relativeRank++;
            }
        }
        handClassification[classIndex++] = holeCards[0] / 4 == holeCards[1] / 4;

        uint32_t key = 0;
        key = 4 * key + handClassification[0];
        key = 4 * key + handClassification[1];
        key = 4 * key + handClassification[2];
        key = 6 * key + handClassification[3];
        key = 2 * key + handClassification[4];
        key = 5 * key + handClassification[5];
        key = 16 * key + handClassification[6];
        key = 7 * key + handClassification[7];
        key = 4 * key + handClassification[8];
        key = 4 * key + handClassification[9];
        key = 7 * key + handClassification[10];
        key = 4 * key + handClassification[11];
        key = 4 * key + handClassification[12];
        key = 2 * key + handClassification[13];
        return key + 1;
    }
};
