#pragma once

#include "solver/multiHand/config.h"
#include "solver/multiHand/engine.h"

// The lookup tables the server's engine reads, loaded once per process.
//
// The same files the trainer's Engine::loadTables() reads, through the same static
// loaders, minus handScores.bin: only setRewards() reads that 535 MB table, and the
// server never reaches a showdown. Kept apart from Engine's own statics so that
// including engine.h (for its loaders, and in the equivalence test for Engine itself)
// never loads anything the server does not ask for.
struct ServerTables {
    inline static std::array<std::array<uint32_t, 8>, 53> binomials;
    inline static std::vector<uint16_t> boardClassTable;
    inline static std::vector<uint16_t> boardClassBasicTable;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetas;
    inline static ankerl::unordered_dense::map<uint64_t, uint8_t> handMetasBasic;
    inline static std::array<uint64_t, COMBO_COUNT> preflopHandClasses;

    static void load() {
        static std::once_flag loadFlag;
        std::call_once(loadFlag, [] {
            Engine::loadTable("solver/handAnalysis/boardClasses.bin", boardClassTable);
            Engine::loadTable("solver/handAnalysis/boardClassesBasic.bin", boardClassBasicTable);
            if (*std::max_element(boardClassTable.begin(), boardClassTable.end()) + 169 > 0xFFFF
                || *std::max_element(boardClassBasicTable.begin(), boardClassBasicTable.end()) + 169 > 0xFFFF) {
                throw std::runtime_error("board class + 169 overflows the 16-bit key field");
            }
            Engine::loadHandMetas("solver/handAnalysis/handMetasBasic.bin", handMetasBasic);
            Engine::loadHandMetas("solver/handAnalysis/handMetas.bin", handMetas);

            // As Engine::loadTables builds them.
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
            for (int i = 0; i < 51; i++) {
                for (int j = i + 1; j < 52; j++) {
                    preflopHandClasses[holePairIndex(i, j)] = (i / 4) * (25 - (i / 4)) / 2
                        + (j / 4) + (90 - (i / 4)) * (i % 4 == j % 4);
                }
            }
        });
    }
};
