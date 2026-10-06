#include "solver/include/library.h"

struct Dealer {

    std::vector<int> deck;

    std::array<int, 3> flopCardIndexes;
    std::array<int, 3> flopCards;
    int flopIndex;    

    std::array<int, 1> turnCardIndexes;
    std::array<int, 1> turnCards;
    int turnIndex;    

    std::array<int, 1> riverCardIndexes;
    std::array<int, 1> riverCards;
    int riverIndex;    

    std::array<int, 5> boardCards;

    std::array<int, 2> holeCardIndexes;
    std::array<int, 2> holeCards;
    int holeIndex;

    std::array<int, 14> cardRanks;
    std::array<int, 4> cardSuits;
    int flushSuit;
    std::array<int, 14> suitedCardRanks;

    std::array<std::array<uint32_t, 8>, 53> binomials;

    std::array<int, 12> handRankThresholds;
    std::array<int, 991> handRankBuckets;

    Dealer() {
        deck.resize(52);
        std::iota(deck.begin(), deck.end(), 0);
        flopCardIndexes = {0, 1, 2};
        flopIndex = 2;
        std::fill(cardRanks.begin(), cardRanks.end(), 0);
        std::fill(cardSuits.begin(), cardSuits.end(), 0);
        flushSuit = -1;
        std::fill(suitedCardRanks.begin(), suitedCardRanks.end(), 0);
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
        for (int i = 0; i < 991; i++) {
            handRankBuckets[i] = i / 6;
        }
        handRankBuckets[990] = 164;
    }  

    bool flopGen() {
        while (flopCardIndexes[flopIndex] > 49 + flopIndex) {
            if (flopIndex > 0) {
                flopIndex--;
                flopCardIndexes[flopIndex]++;
            } else {
                return false;
            }
        }
        while (flopIndex < 2) {
            flopIndex++;
            flopCardIndexes[flopIndex] = flopCardIndexes[flopIndex - 1] + 1;
        }
        for (int i = 0; i < 3; i++) {
            flopCards[i] = deck[flopCardIndexes[i]];
            boardCards[i] = flopCards[i];
        }
        std::iota(turnCardIndexes.begin(), turnCardIndexes.end(), 0);
        turnIndex = 0;
        int q = 0;
        for (int i = 0; i < 52; i++) {
            if (q < 3 && flopCards[q] == deck[i]) {
                q++; 
            } else {
                deck[i - q] = deck[i];
            }
        }
        deck.resize(49);
        return true;
    }

    void flopSet() {
        deck.resize(52);
        int q = 2;
        for (int i = 51; i >= 0; i--) {
            if (q >= 0) {
                if (i - q - 1 < 0) {
                    deck[i] = flopCards[q];
                    q--;
                } else if (flopCards[q] > deck[i - q - 1]) {
                    deck[i] = flopCards[q];
                    q--;
                } else {
                    deck[i] = deck[i - q - 1];
                }
            } else {
                deck[i] = deck[i - q - 1];
            }
        }
        flopCardIndexes[flopIndex]++;
    }

    bool turnGen() {
        while (turnCardIndexes[turnIndex] > 48 + turnIndex) {
            if (turnIndex > 0) {
                turnIndex--;
                turnCardIndexes[turnIndex]++;
            } else {
                return false;
            }
        }
        while (turnIndex < 0) {
            turnIndex++;
            turnCardIndexes[turnIndex] = turnCardIndexes[turnIndex - 1] + 1;
        }
        for (int i = 0; i < 1; i++) {
            turnCards[i] = deck[turnCardIndexes[i]];
            boardCards[i + 3] = turnCards[i];
        }
        std::iota(riverCardIndexes.begin(), riverCardIndexes.end(), 0);
        riverIndex = 0;
        int q = 0;
        for (int i = 0; i < 49; i++) {
            if (q < 1 && turnCards[q] == deck[i]) {
                q++; 
            } else {
                deck[i - q] = deck[i];
            }
        }
        deck.resize(48);
        return true;
    }

    void turnSet() {
        deck.resize(49);
        int q = 0;
        for (int i = 48; i >= 0; i--) {
            if (q >= 0) {
                if (i - q - 1 < 0) {
                    deck[i] = turnCards[q];
                    q--;
                } else if (turnCards[q] > deck[i - q - 1]) {
                    deck[i] = turnCards[q];
                    q--;
                } else {
                    deck[i] = deck[i - q - 1];
                }
            } else {
                deck[i] = deck[i - q - 1];
            }
        }
        turnCardIndexes[turnIndex]++;
    }

    bool riverGen() {
        while (riverCardIndexes[riverIndex] > 47 + riverIndex) {
            if (riverIndex > 0) {
                riverIndex--;
                riverCardIndexes[riverIndex]++;
            } else {
                return false;
            }
        }
        while (riverIndex < 0) {
            riverIndex++;
            riverCardIndexes[riverIndex] = riverCardIndexes[riverIndex - 1] + 1;
        }
        for (int i = 0; i < 1; i++) {
            riverCards[i] = deck[riverCardIndexes[i]];
            boardCards[i + 4] = riverCards[i];
        }
        std::iota(holeCardIndexes.begin(), holeCardIndexes.end(), 0);
        holeIndex = 1;
        int q = 0;
        for (int i = 0; i < 48; i++) {
            if (q < 1 && riverCards[q] == deck[i]) {
                q++; 
            } else {
                deck[i - q] = deck[i];
            }
        }
        deck.resize(47);
        for (int u : boardCards) {
            cardRanks[u / 4]++;
            cardSuits[u % 4]++;
        }
        for (int i = 0; i < 4; i++) {
            if (cardSuits[i] > 2) {
                flushSuit = i;
                break;
            }
        }
        for (int u : boardCards) {
            if (u % 4 == flushSuit) {
                suitedCardRanks[u / 4]++;
            }
        }
        return true;
    }

    void riverSet() {
        for (int u : boardCards) {
            cardRanks[u / 4]--;
            cardSuits[u % 4]--;
        }
        for (int u : boardCards) {
            if (u % 4 == flushSuit) {
                suitedCardRanks[u / 4]--;
            }
        }
        flushSuit = -1;
        deck.resize(48);
        int q = 0;
        for (int i = 47; i >= 0; i--) {
            if (q >= 0) {
                if (i - q - 1 < 0) {
                    deck[i] = riverCards[q];
                    q--;
                } else if (riverCards[q] > deck[i - q - 1]) {
                    deck[i] = riverCards[q];
                    q--;
                } else {
                    deck[i] = deck[i - q - 1];
                }
            } else {
                deck[i] = deck[i - q - 1];
            }
        }
        riverCardIndexes[riverIndex]++;
    }

    bool holeGen() {
        while (holeCardIndexes[holeIndex] > 45 + holeIndex) {
            if (holeIndex > 0) {
                holeIndex--;
                holeCardIndexes[holeIndex]++;
            } else {
                return false;
            }
        }
        while (holeIndex < 1) {
            holeIndex++;
            holeCardIndexes[holeIndex] = holeCardIndexes[holeIndex - 1] + 1;
        }
        for (int i = 0; i < 2; i++) {
            holeCards[i] = deck[holeCardIndexes[i]];
        }
        for (int u : holeCards) {
            cardRanks[u / 4]++;
            cardSuits[u % 4]++;
        }
        if (flushSuit != -1) {
            for (int u : holeCards) {
                if (u % 4 == flushSuit) {
                    suitedCardRanks[u / 4]++;
                }
            }
        }
        return true;
    }

    void holeSet() {
        for (int u : holeCards) {
            cardRanks[u / 4]--;
            cardSuits[u % 4]--;
        }
        if (flushSuit != -1) {
            for (int u : holeCards) {
                if (u % 4 == flushSuit) {
                    suitedCardRanks[u / 4]--;
                }
            }
        }
        holeCardIndexes[holeIndex]++;
    }

    uint32_t getHandIndex() {
        std::array<int, 7> handCards = {holeCards[0], holeCards[1], boardCards[0], 
            boardCards[1], boardCards[2], boardCards[3], boardCards[4]};
        std::sort(handCards.begin(), handCards.end(), [](int a, int b) {
            return a < b;
        });
        uint32_t handIndex = 0;
        int u = -1;
        for (int i = 0; i < 7; i++) {
            for (int j = u + 1; j < handCards[i]; j++) {
                handIndex += binomials[51 - j][7 - i - 1];
            }
            u = handCards[i];
        }
        return handIndex;
    }

    uint32_t getBoardIndex(int street) {
        uint32_t boardIndex = 0;
        for (int x = 0; x < boardCards[0]; ++x) {
            boardIndex += binomials[51 - x][2];
        }
        for (int x = boardCards[0] + 1; x < boardCards[1]; ++x) {
            boardIndex += 51 - x;
        }
        boardIndex += boardCards[2] - boardCards[1] - 1;
        boardIndex *= 2757;
        if (street == 1) {
            return boardIndex;
        }
        boardIndex += 1 + boardCards[3] * 53;
        if (street == 2) {
            return boardIndex;
        }
        boardIndex += 1 + boardCards[4];
        return boardIndex;
    }

    int scoreHand() {
        int handScore = 0;

        if (flushSuit != -1 && cardSuits[flushSuit] > 4) {
            suitedCardRanks[13] = 0;
            if (suitedCardRanks[0] > 0) {
                suitedCardRanks[13] = 1;
            }
            for (int i = 0; i < 10; i++) {
                for (int j = i; j < i + 5; j++) {
                    if (suitedCardRanks[j] == 0) {
                        i += j - i;
                        break;
                    } else if (j == i + 4) {
                        handScore = 8 * pow(13, 5) + (12 - i);
                        return handScore;
                    }
                }
            }
            handScore = 5 * pow(13, 5);
            int i = 0;
            int j = 0;
            while (j < 5) {
                if (suitedCardRanks[i] > 0) {
                    handScore += (12 - i) * pow(13, 4 - j);
                    j++;
                }
                i++;
            }
            return handScore;
        }

        cardRanks[13] = 0;
        if (cardRanks[0] > 0) {
            cardRanks[13] = 1;
        } 
        for (int i = 0; i < 10; i++) {
            for (int j = i; j < i + 5; j++) {
                if (cardRanks[j] == 0) {
                    i += j - i;
                    break;
                } else if (j == i + 4) {
                    handScore = 4 * pow(13, 5) + (12 - i);
                    return handScore;
                }
            }
        }

        int quadRank = -1;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 3) {
                quadRank = i;
                break;
            }
        }
        if (quadRank != -1) {
            handScore = 7 * pow(13, 5) + (12 - quadRank) * 13;
            for (int i = 0; i < 13; i++) {
                if (i != quadRank && cardRanks[i] > 0) {
                    handScore += (12 - i);
                    return handScore;
                }
            }
        }

        int tripRank = -1;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 2) {
                tripRank = i;
                break;
            }
        }
        if (tripRank != -1) {
            int pairRank = -1;
            for (int i = 0; i < 13; i++) {
                if (i != tripRank && cardRanks[i] > 1) {
                    pairRank = i;
                    break;
                }
            }
            if (pairRank != -1) {
                handScore = 6 * pow(13, 5) + (12 - tripRank) * 13 + (12 - pairRank);
                return handScore;
            }
            handScore = 3 * pow(13, 5) + (12 - tripRank) * pow(13, 2);
            int i = 0;
            int j = 0;
            while (j < 2) {
                if (i != tripRank && cardRanks[i] > 0) {
                    handScore += (12 - i) * pow(13, 1 - j);
                    j++;
                }
                i++;
            }
            return handScore;
        }

        int pairRank = -1;
        for (int i = 0; i < 13; i++) {
            if (cardRanks[i] > 1) {
                pairRank = i;
                break;
            }
        }
        if (pairRank != -1) {
            int pair2Rank = -1;
            for (int i = 0; i < 13; i++) {
                if (i != pairRank && cardRanks[i] > 1) {
                    pair2Rank = i;
                    break;
                }
            }
            if (pair2Rank != -1) {
                handScore = 2 * pow(13, 5) + (12 - pairRank) * pow(13, 2) + (12 - pair2Rank) * 13;
                for (int i = 0; i < 13; i++) {
                    if (i != pairRank && i != pair2Rank && cardRanks[i] > 0) {
                        handScore += (12 - i);
                        return handScore;
                    }
                }
            }
            handScore = 1 * pow(13, 5) + (12 - pairRank) * pow(13, 3);
            int i = 0;
            int j = 0;
            while (j < 3) {
                if (i != pairRank && cardRanks[i] > 0) {
                    handScore += (12 - i) * pow(13, 2 - j);
                    j++;
                }
                i++;
            }
            return handScore;
        }

        int i = 0;
        int j = 0;
        while (j < 5) {
            if (cardRanks[i] > 0) {
                handScore += (12 - i) * pow(13, 4 - j);
                j++;
            }
            i++;
        }
        return handScore;  
    }

    std::array<int, 1326> rankHands() {
        std::array<int, 1326> referenceRanks;
        for (int i = 0; i < 1326; i++) {
            referenceRanks[i] = -1;
        }
        std::array<std::array<int, 4>, 1081> handScores;
        int handIndex = 0;
        while (this->holeGen()) {
            handScores[handIndex][1] = this->holeCards[0];
            handScores[handIndex][2] = this->holeCards[1];
            handScores[handIndex][3] = this->scoreHand();
            handScores[handIndex][0] = (101 - handScores[handIndex][1])
                * handScores[handIndex][1] / 2 + handScores[handIndex][2] - 1;
            handIndex++;
            this->holeSet();
        }
        std::sort(std::begin(handScores), std::end(handScores), 
            [](const std::array<int, 4>& a, const std::array<int, 4>& b) {
            return a[3] > b[3];
        });
        std::array<int, 1326> cardOverlaps = {0};
        int handRankIndex = 0;
        int handRank = 0;
        for (int i = 0; i < 1081; i++) {
            if (i == 0) {
                handRank = 0;
            } else if (handScores[i][3] == handScores[i - 1][3]) {
                handRank = handRankIndex;
                handRank -= cardOverlaps[handScores[i][1]];
                handRank -= cardOverlaps[handScores[i][2]];
            } else {  
                for (int j = handRankIndex; j < i; j++) {
                    cardOverlaps[handScores[j][1]]++;
                    cardOverlaps[handScores[j][2]]++;
                }
                handRankIndex = i;
                handRank = i;
                handRank -= cardOverlaps[handScores[i][1]];
                handRank -= cardOverlaps[handScores[i][2]];
            }
            referenceRanks[handScores[i][0]] = handRankBuckets[handRank];
        }
        return referenceRanks;
    }
};
