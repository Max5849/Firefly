#pragma once

// solver/trainer/engine.h brings config.h, library.h and HandClusterer, which has no
// include guard, so nothing else in the server includes handClusterer.h.
#include "solver/trainer/config.h"
#include "solver/trainer/engine.h"

// The lookup tables the server's engine reads, loaded once per process.
//
// The same files the trainer's Engine::loadTables() reads, through the same static
// loaders, minus handScores.bin: only showdown() reads that 535 MB table, and the server
// never reaches a showdown. Each metas stage loads on first use (requireStage), so a
// server whose strategies are all one stage never holds the others' tables, and a stage
// whose tables have not been built yet fails only the requests that ask for it. Kept
// apart from Engine's own statics so that including engine.h (for its loaders, and in
// the equivalence test for Engine itself) never loads anything the server does not ask
// for.
struct ServerTables {
    inline static std::array<std::array<uint32_t, 8>, 53> binomials;
    inline static std::array<uint64_t, COMBO_COUNT> preflopHandClasses;
    // Indexed by metas stage; empty until requireStage(stage) has run.
    inline static std::array<std::vector<uint16_t>, METAS_STAGE_COUNT> boardClassTables;
    inline static std::array<ankerl::unordered_dense::map<uint64_t, uint8_t>, METAS_STAGE_COUNT> handMetas;

    // The tables every stage shares: binomials and the 169 classes, as
    // Engine::loadTables builds them.
    static void load() {
        static std::once_flag loadFlag;
        std::call_once(loadFlag, [] {
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
            // Engine::loadTables' formula.
            for (int i = 0; i < 51; i++) {
                for (int j = i + 1; j < 52; j++) {
                    preflopHandClasses[holePairIndex(i, j)] = (i / 4) * (25 - (i / 4)) / 2
                        + (j / 4) + (90 - (i / 4)) * (i % 4 == j % 4);
                }
            }
        });
    }

    // Loads one metas stage's tables, with Engine::loadTables' checks. Throws when a file
    // is missing or malformed; std::call_once lets a later call try again.
    static void requireStage(int stage) {
        if (stage < 0 || stage >= METAS_STAGE_COUNT) {
            throw std::runtime_error("metas stage " + std::to_string(stage) + " does not exist");
        }
        load();
        static std::array<std::once_flag, METAS_STAGE_COUNT> stageFlags;
        std::call_once(stageFlags[stage], [stage] {
            std::vector<uint16_t> table;
            Engine::loadTable(stagePath("boardClasses", stage), table);
            if (*std::max_element(table.begin(), table.end()) + 169 > BOARD_CLASS_MASK) {
                throw std::runtime_error(stagePath("boardClasses", stage)
                    + ": board class + 169 overflows the 15-bit key field");
            }
            ankerl::unordered_dense::map<uint64_t, uint8_t> metas;
            Engine::loadHandMetas(stagePath("handMetas", stage), metas);
            for (const auto& [key, meta] : metas) {
                if (meta > HAND_META_MASK) {
                    throw std::runtime_error(stagePath("handMetas", stage)
                        + ": a hand meta overflows the 6-bit key field");
                }
            }
            boardClassTables[stage] = std::move(table);
            handMetas[stage] = std::move(metas);
        });
    }

    static bool stageLoaded(int stage) {
        return stage >= 0 && stage < METAS_STAGE_COUNT && !boardClassTables[stage].empty();
    }
};
