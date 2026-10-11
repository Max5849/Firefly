#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/handClustererBasic.h"
#include "solver/handAnalysis/dealer.h"

bool BASIC = true;

int main() {
    Dealer dealer;
    HandClusterer handClusterer;
    HandClustererBasic handClustererBasic;
    std::vector<uint16_t> boardClasses(60929700);
    std::unordered_map<uint64_t, int> boardClassIndexes;
    int boardClassIndex = 0;

    while (dealer.flopGen()) {
        uint32_t flopClass;
        if (!BASIC) {
            flopClass = handClusterer.classifyFlop(dealer.boardCards);
        } else {
            flopClass = handClustererBasic.classifyFlop(dealer.boardCards);
        }
        uint64_t boardClassKey = (uint64_t)flopClass | ((uint64_t)0 << 18) | ((uint64_t)0 << 39);
        if (boardClassIndexes.find(boardClassKey) != boardClassIndexes.end()) {
            boardClasses[dealer.getBoardIndex(1)] = boardClassIndexes[boardClassKey];
        } else {
            boardClassIndexes[boardClassKey] = boardClassIndex;
            boardClasses[dealer.getBoardIndex(1)] = boardClassIndex;
            boardClassIndex++;
        }
        while (dealer.turnGen()) {
            uint32_t turnClass;
            if (!BASIC) {
                handClusterer.classifyFlop(dealer.boardCards);
                turnClass = handClusterer.classifyTurn(dealer.boardCards);
            } else {
                handClustererBasic.classifyFlop(dealer.boardCards);
                turnClass = handClustererBasic.classifyTurn(dealer.boardCards);
            }
            boardClassKey = (uint64_t)flopClass | ((uint64_t)turnClass << 18) | ((uint64_t)0 << 39);
            if (boardClassIndexes.find(boardClassKey) != boardClassIndexes.end()) {
                boardClasses[dealer.getBoardIndex(2)] = boardClassIndexes[boardClassKey];
            } else {
                boardClassIndexes[boardClassKey] = boardClassIndex;
                boardClasses[dealer.getBoardIndex(2)] = boardClassIndex;
                boardClassIndex++;
            }
            while (dealer.riverGen()) {
                uint32_t riverClass;
                if (!BASIC) {
                    handClusterer.classifyFlop(dealer.boardCards);
                    handClusterer.classifyTurn(dealer.boardCards);
                    riverClass = handClusterer.classifyRiver(dealer.boardCards);
                } else {
                    handClustererBasic.classifyFlop(dealer.boardCards);
                    handClustererBasic.classifyTurn(dealer.boardCards);
                    riverClass = handClustererBasic.classifyRiver(dealer.boardCards);
                }
                boardClassKey = (uint64_t)flopClass | ((uint64_t)turnClass << 18) | ((uint64_t)riverClass << 39);
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
    std::string name = !BASIC ? "solver/handAnalysis/boardClasses.bin" : "solver/handAnalysis/boardClassesBasic.bin";
    std::ofstream writeFile(name, std::ios::binary);
    writeFile.write(reinterpret_cast<char*>(boardClasses.data()), boardClasses.size() * sizeof(uint16_t));
    writeFile.close();
}
