#include "include/library.h"
#include "handClusterer.h"
#include "dealer.h"

int main() {
    Dealer dealer;
    HandClusterer handClusterer;
    std::vector<uint16_t> boardClasses(60929700);
    std::unordered_map<uint64_t, int> boardClassIndexes;
    int boardClassIndex = 0;

    while (dealer.flopGen()) {
        uint32_t flopClass = handClusterer.classifyFlop(dealer.boardCards);
        uint64_t boardClassKey = (uint64_t)flopClass | ((uint64_t)0 << 21) | ((uint64_t)0 << 42);
        if (boardClassIndexes.find(boardClassKey) != boardClassIndexes.end()) {
            boardClasses[dealer.getBoardIndex(1)] = boardClassIndexes[boardClassKey];
        } else {
            boardClassIndexes[boardClassKey] = boardClassIndex;
            boardClasses[dealer.getBoardIndex(1)] = boardClassIndex;
            boardClassIndex++;
        }
        while (dealer.turnGen()) {
            handClusterer.classifyFlop(dealer.boardCards);
            uint32_t turnClass = handClusterer.classifyTurn(dealer.boardCards);
            boardClassKey = (uint64_t)flopClass | ((uint64_t)turnClass << 21) | ((uint64_t)0 << 42);
            if (boardClassIndexes.find(boardClassKey) != boardClassIndexes.end()) {
                boardClasses[dealer.getBoardIndex(2)] = boardClassIndexes[boardClassKey];
            } else {
                boardClassIndexes[boardClassKey] = boardClassIndex;
                boardClasses[dealer.getBoardIndex(2)] = boardClassIndex;
                boardClassIndex++;
            }
            while (dealer.riverGen()) {
                handClusterer.classifyFlop(dealer.boardCards);
                handClusterer.classifyTurn(dealer.boardCards);
                uint32_t riverClass = handClusterer.classifyRiver(dealer.boardCards);
                boardClassKey = (uint64_t)flopClass | ((uint64_t)turnClass << 21) | ((uint64_t)riverClass << 42);
                if (boardClassIndexes.find(boardClassKey) != boardClassIndexes.end()) {
                    boardClasses[dealer.getBoardIndex(3)] = boardClassIndexes[boardClassKey];
                } else {
                    boardClassIndexes[boardClassKey] = boardClassIndex;
                    boardClasses[dealer.getBoardIndex(3)] = boardClassIndex;
                    boardClassIndex++;
                }
                dealer.riverSet();
            }
            dealer.turnSet();
        }
        dealer.flopSet();
    }
    std::cout << boardClassIndex;
    std::ofstream writeFile("boardClasses.bin", std::ios::binary);
    writeFile.write(reinterpret_cast<char*>(boardClasses.data()), boardClasses.size() * sizeof(uint16_t));
    writeFile.close();
}
