#include "include/library.h"

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

    HandClusterer() {
        cardRankBuckets = {0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3};
        holeCardRankBuckets = {0, 1, 2, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4};
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
            if (flopClassification[0] > 1) {
                straightState = 0;
            }
        } else if (straightFillCount < 2) {
            straightState = 2;
            if (flopClassification[0] > 1) {
                straightState = 0;
            }
        } else if (straightFillCount < 3) {
            straightState = 3;
        } else {
            straightState = 4;
        }
        flopClassification[classIndex++] = straightState;
        
        int highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 0) {
                highCard = i;
                flopClassification[classIndex++] = cardRankBuckets[i];
                break;
            }
        }
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 0 && i != highCard) {
                flopClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                flopClassification[classIndex++] = cardRankBuckets[highCard];
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
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
        key = 4 * key + flopClassification[2];
        key = 4 * key + flopClassification[3];
        key = 4 * key + flopClassification[4];
        key = 4 * key + flopClassification[5];
        key = 4 * key + flopClassification[6];
        key = 4 * key + flopClassification[7];
        return key + 1;
    }    

    uint32_t classifyTurn(const std::array<int, 5>& boardCards) {
        std::array<int, 8> turnClassification;
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
                if (turnClassification[0] > 1) {
                    straightState = 0;
                }
            } else if (straightFillCount < 3) {
                straightState = 3;
            } else {
                straightState = 4;
            }
            if (turnClassification[0] > 2) {
                straightState = 0;
            }
        } else {
            if (straightOneFillCount < 2) {
                if (straightOneCompleteCount < 1) {
                    straightState = 5;
                } else {
                    straightState = 6;
                }
            } else {
                straightState = 7;
            }
        } 
        turnClassification[classIndex++] = straightState;
        
        int highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 0) {
                highCard = i;
                turnClassification[classIndex++] = cardRankBuckets[i];
                break;
            }
        }
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 0 && i != highCard) {
                turnClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                turnClassification[classIndex++] = cardRankBuckets[highCard];
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
                highCard = i;
                turnClassification[classIndex++] = cardRankBuckets[i];
                turnClassification[classIndex++] = cardRanks[i] - 1;
                break;
            } else if (i == 12) {
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
        key = 8 * key + turnClassification[1];
        key = 4 * key + turnClassification[2];
        key = 4 * key + turnClassification[3];
        key = 4 * key + turnClassification[4];
        key = 4 * key + turnClassification[5];
        key = 4 * key + turnClassification[6];
        key = 4 * key + turnClassification[7];
        return key + 1;
    }     

    uint32_t classifyRiver(const std::array<int, 5>& boardCards) {
        std::array<int, 9> riverClassification;
        int classIndex = 0;

        bool backdoorFlush = false;
        for (int i = 4; i < 5; i++) {
            cardRanks[boardCards[i] / 4]++;
            cardSuits[boardCards[i] % 4]++;
            if (cardSuits[boardCards[i] % 4] == 3 && flushSuit == -1) {
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
                    if (riverClassification[0] > 1 && riverClassification[0] != 5) {
                        straightState = 0;
                    }
                } else if (straightFillCount < 3) {
                    straightState = 3;
                } else {
                    straightState = 4;
                }
                if (riverClassification[0] > 2 && riverClassification[0] != 5) {
                    straightState = 0;
                }
            } else {
                if (straightOneFillCount < 2) {
                    if (straightOneCompleteCount < 1) {
                        straightState = 5;
                    } else {
                        straightState = 6;
                    }
                } else {
                    straightState = 7;
                }
            } 
        } else {
            if (straightOneCompleteCount < 1) {
                straightState = 8;
            } else {
                straightState = 9;
            }
        }
        if (riverClassification[0] == 4) {
            straightState = 0;
        }
        riverClassification[classIndex++] = straightState;
        
        int highCard = 0;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 0) {
                highCard = i;
                riverClassification[classIndex++] = cardRankBuckets[i];
                break;
            }
        }
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 0 && i != highCard) {
                riverClassification[classIndex++] = cardRankBuckets[i];
                break;
            } else if (i == 12) {
                riverClassification[classIndex++] = cardRankBuckets[highCard];
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
                highCard = i;
                riverClassification[classIndex++] = cardRankBuckets[i];
                riverClassification[classIndex++] = cardRanks[i] - 1;
                if (cardRanks[i] > 2) {
                    riverClassification[0] = 0;
                    riverClassification[2] = 0;
                }
                break;
            } else if (i == 12) {
                riverClassification[classIndex++] = 0;
                riverClassification[classIndex++] = 0;
            }
        }

        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1 && i != highCard) {
                riverClassification[classIndex++] = cardRankBuckets[i];
                riverClassification[classIndex++] = cardRanks[i] - 1;
                if (cardRanks[i] > 2) {
                    riverClassification[0] = 0;
                    riverClassification[2] = 0;
                }
                break;
            } else if (i == 12) {
                riverClassification[classIndex++] = 0;
                riverClassification[classIndex++] = 0;
            }
        }

        uint32_t key = 0;
        key = 6 * key + riverClassification[0];
        key = 4 * key + riverClassification[1];
        key = 10 * key + riverClassification[2];
        key = 4 * key + riverClassification[3];
        key = 4 * key + riverClassification[4];
        key = 4 * key + riverClassification[5];
        key = 4 * key + riverClassification[6];
        key = 4 * key + riverClassification[7];
        key = 4 * key + riverClassification[8];
        return key + 1;
    }   

    uint32_t classifyHand(const std::array<int, 5>& boardCards, const std::array<int, 2>& holeCards, int street) {
        std::array<int, 11> handClassification;
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
                flushState = std::min(cardSuits[i] + holeCardSuits[i] - 1, 4);
                holeFlushSuit = i;
                break;
            }
        }
        
        int suitedStraightState = 0;
        int suitedStraightRank = 0;
        if (flushState == 4) {
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
            for (int k = 0; k < 10; k++) {
                if (suitedCardRanks[k] + suitedHoleCardRanks[k] > 0) {
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
                for (int l = k; l < k + 5; l++) {
                    if (suitedCardRanks[l] + suitedHoleCardRanks[l] == 0) {
                        singleSuitedStraightState++; 
                    } else if (suitedCardRanks[l] == 0) {
                        holeCardCount++;
                    }
                }
                if (holeCardCount > 0 && singleSuitedStraightState == 0) {
                    suitedStraightState = 1;
                    bool fullRank = false;
                    for (int m = std::max(k - 4, 0); m < k; m++) {
                        if (suitedCardRanks[m] > 0 || (m >= k && suitedHoleCardRanks[m] == 0)) {
                            if (fullRank) {
                                continue;
                            } else {
                                fullRank = true;
                            }
                        } else if (fullRank) {
                            fullRank = false;
                            continue;
                        } 
                        int singleSuitedStraightRank = 0;
                        for (int n = m; n < m + 5; n++) {
                            if (suitedCardRanks[n] == 0 && !(n >= k && suitedHoleCardRanks[n] == 0)) {
                                singleSuitedStraightRank++; 
                            }                     
                        }
                        if (singleSuitedStraightRank < 2) {
                            suitedStraightRank = 1;
                        }
                    }
                }
            }
        } 
        handClassification[classIndex++] = suitedStraightState + suitedStraightRank;

        int flushRank = 0;
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
            if (overCount > 4 || (flushState == 2 && highCard - overCount > 2)) {
                flushState = 0;
            } else {
                if (flushState == 2 && holeCards[0] % 4 != holeCards[1] % 4) {
                    flushState = 1;
                }
                if (highCard - overCount < 1) {
                    flushRank = 0;
                } else if (highCard - overCount < 3) {
                    flushRank = 1;
                } else if (highCard - overCount < 6) {
                    flushRank = 2;
                } else {
                    flushRank = 3;
                }
            }
        }
        handClassification[classIndex++] = flushState;
        handClassification[classIndex++] = flushRank;

        if (holeCardRanks[0] > 0) {
            holeCardRanks[13] = 1;
        }
        int straightState = 0;
        int straightRank = 0;
        std::array<int, 3> straightStates = {0, 0, 0};
        std::array<int, 3> straightRanks = {0, 0, 0};
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
                straightStates[2 - singleStraightState]++;
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
                    if (singleStraightRank < 3) {
                        straightRanks[2 - singleStraightState] = std::max
                            (straightRanks[2 - singleStraightState], 3 - singleStraightRank);
                    }
                }
            }
        }
        if (straightStates[2] > 0) {
            straightState = 4;
            straightRank = std::min(straightRanks[2], 2);
        } else if (straightStates[1] > 0) {
            if (straightStates[1] == 1) {
                straightState = 3;
                straightRank = std::min(straightRanks[1], 2);
            } else if (straightStates[1] == 2) {
                straightState = 2;
                straightRank = std::min(straightRanks[1], 2);
            }
        } else if (straightStates[0] > 2 && straightRanks[0] < 2) {
            straightState = 1;
            straightRank = std::min(straightRanks[0], 2);
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
                handClassification[classIndex++] = std::min(relativeRank, 2);
                if (cardRanks[i] > 0) {
                    handClassification[classIndex++] = cardRanks[i] + holeCardRanks[i];
                } else {
                    handClassification[classIndex++] = holeCardRanks[i] - 1;
                }
            }
            if (cardRanks[i] > 0) {
                relativeRank++;
            }
        }

        uint32_t key = 0;
        key = 3 * key + handClassification[0];
        key = 5 * key + handClassification[1];
        key = 4 * key + handClassification[2];
        key = 5 * key + handClassification[3];
        key = 3 * key + handClassification[4];
        key = 5 * key + handClassification[5];
        key = 3 * key + handClassification[6];
        key = 5 * key + handClassification[7];
        key = 5 * key + handClassification[8];
        key = 3 * key + handClassification[9];
        key = 5 * key + handClassification[10];
        return key + 1;
    }
};
