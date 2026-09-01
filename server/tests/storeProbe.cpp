// Prints what StrategyStore returns for each hex key on the command line, so an
// independent reader of strategy_<spec>.bin can be diffed against the mmap path.
//
//   build/storeProbe 0x1234... 0xabcd...

#include "solver/config.h"
#include "server/strategyStore.h"

#include <cstdlib>

int main(int argc, char** argv) {
    try {
        StrategyStore store;
        store.open(defaultStrategyPath(), defaultIndexPath());
        std::cout << std::setprecision(17);
        for (int i = 1; i < argc; i++) {
            uint64_t key = std::strtoull(argv[i], nullptr, 0);
            StrategyRecord record;
            if (!store.lookup(key, record)) {
                std::cout << key << " MISS" << std::endl;
                continue;
            }
            std::cout << key;
            for (int j = 0; j < 4; j++) {
                std::cout << " " << record.weights[j];
            }
            std::cout << " " << record.visits << std::endl;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
