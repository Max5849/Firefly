#include "solver/include/library.h"

struct HandScorer {
    std::vector<int> deck;
    std::array<int, 7> cards;
    int cardIndex;
    std::array<int, 14> cardRanks;
    std::array<int, 4> cardSuits;
    std::array<int, 14> suitedCardRanks;
    int flushSuit;
    
    HandScorer() {
        deck.resize(52);
        std::iota(deck.begin(), deck.end(), 0);
        std::iota(cards.begin(), cards.end(), 0);
        cardIndex = 6;
        std::fill(cardRanks.begin(), cardRanks.end(), 0);
        std::fill(cardSuits.begin(), cardSuits.end(), 0);
        std::fill(suitedCardRanks.begin(), suitedCardRanks.end(), 0);
    }

    bool cardGen() {
        while (cards[cardIndex] > 45 + cardIndex) {
            if (cardIndex > 0) {
                cardIndex--;
                cards[cardIndex]++;
            } else {
                return false;
            }
        }
        while (cardIndex < 6) {
            cardIndex++;
            cards[cardIndex] = cards[cardIndex - 1] + 1;
        }
        for (int u : cards) {
            cardRanks[u / 4]++;
            cardSuits[u % 4]++;
        }
        flushSuit = -1;
        for (int i = 0; i < 4; i++) {
            if (cardSuits[i] > 4) {
                flushSuit = i;
                break;
            }
        }
        if (flushSuit != -1) {
            for (int u : cards) {
                if (u % 4 == flushSuit) {
                    suitedCardRanks[u / 4]++;
                }
            }
        }
        return true;
    }

    void cardSet() {
        if (flushSuit != -1) {
            for (int u : cards) {
                if (u % 4 == flushSuit) {
                    suitedCardRanks[u / 4]--;
                }
            }
        }
        for (int u : cards) {
            cardRanks[u / 4]--;
            cardSuits[u % 4]--;
        }
        cards[cardIndex]++;
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
};

int main() {
    HandScorer handScorer;
    std::vector<uint32_t> handScores(133784560);
    int i = 0;
    while (handScorer.cardGen()) {
        u_int32_t handScore = handScorer.scoreHand();
        handScores[i] = handScore;
        i++;
        if (i % 1000000 == 0) {
            std::cout << i << " / 133784560" << std::endl;
        }
        handScorer.cardSet();
    }
    std::ofstream writeFile("handScores.bin", std::ios::binary);
    writeFile.write(reinterpret_cast<char*>(handScores.data()), handScores.size() * sizeof(uint32_t));
    writeFile.close();
    return 0;
}
