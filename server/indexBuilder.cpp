// Builds the sidecar hash index for a strategy file:
//
//   build/indexBuilder [strategy.bin [strategy.idx]]
//
// Run once after copying a strategy file to the machine, and again whenever the
// solver writes a new one. The server refuses to start if the two disagree.

#include "solver/config.h"
#include "server/strategyStore.h"

int main(int argc, char** argv) {
    try {
        std::string strategyPath = argc > 1 ? argv[1] : defaultStrategyPath();
        std::string indexPath = argc > 2 ? argv[2] : defaultIndexPath();

        uint64_t records = strategyRecordCount(strategyPath);
        uint64_t slotBits = strategySlotBits(records);
        uint64_t slotCount = 1ULL << slotBits;
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Strategy: " << strategyPath << "\n"
                  << "Records: " << records << "\n"
                  << "Slots: " << slotCount << " ("
                  << (slotCount * sizeof(uint64_t) / (1024.0 * 1024.0)) << " MB, load "
                  << (100.0 * double(records) / double(slotCount)) << "%)" << std::endl;

        // Held in memory during the build and written once. At the current scale
        // this is 268 MB, and it grows only as the strategy does.
        std::vector<uint64_t> slots(slotCount, STRATEGY_SLOT_EMPTY);

        MappedFile strategyMap;
        strategyMap.open(strategyPath);

        uint64_t probes = 0;
        for (uint64_t i = 0; i < records; i++) {
            uint64_t key;
            std::memcpy(&key, strategyMap.data + STRATEGY_HEADER_BYTES
                + i * STRATEGY_RECORD_BYTES, sizeof(key));
            uint64_t hash = mix64(key);
            uint64_t slot = hash & (slotCount - 1);
            for (;;) {
                probes++;
                uint64_t word = slots[slot];
                if (word == STRATEGY_SLOT_EMPTY) {
                    slots[slot] = (uint64_t(strategySlotTag(hash)) << 32) | (i + 1);
                    break;
                }
                // A duplicate key would split one state's statistics across two
                // records, and whichever the index kept would be arbitrary.
                if (uint32_t(word >> 32) == strategySlotTag(hash)) {
                    uint64_t other = uint32_t(word) - 1;
                    uint64_t otherKey;
                    std::memcpy(&otherKey, strategyMap.data + STRATEGY_HEADER_BYTES
                        + other * STRATEGY_RECORD_BYTES, sizeof(otherKey));
                    if (otherKey == key) {
                        throw std::runtime_error("duplicate key at records "
                            + std::to_string(other) + " and " + std::to_string(i));
                    }
                }
                slot = (slot + 1) & (slotCount - 1);
            }
        }
        std::cout << "Mean probes per insert: "
                  << (double(probes) / double(records)) << std::endl;

        StrategyIndexHeader header{};
        std::memcpy(header.magic, STRATEGY_INDEX_MAGIC, 8);
        header.records = records;
        header.slotBits = slotBits;
        header.recordBytes = STRATEGY_RECORD_BYTES;

        CheckedWriter writeFile(indexPath);
        writeFile.write(&header, sizeof(header));
        writeFile.write(slots.data(), slots.size() * sizeof(uint64_t));
        writeFile.commit();
        std::cout << "Wrote " << indexPath << std::endl;

        // Read the index back through the same path the server uses and confirm
        // every record is reachable at its own key. A build that silently lost
        // records would otherwise show up as an unexplained miss rate.
        StrategyStore store;
        store.open(strategyPath, indexPath);
        StrategyRecord record;
        for (uint64_t i = 0; i < records; i++) {
            uint64_t key;
            std::memcpy(&key, strategyMap.data + STRATEGY_HEADER_BYTES
                + i * STRATEGY_RECORD_BYTES, sizeof(key));
            if (!store.lookup(key, record) || record.key != key) {
                throw std::runtime_error("record " + std::to_string(i)
                    + " is not reachable through the index it was just written to");
            }
        }
        std::cout << "Verified " << records << " keys" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
