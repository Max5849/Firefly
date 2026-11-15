// Prints what StrategyStore returns for each hex key on the command line, so an
// independent reader of strategy_<spec>.bin can be diffed against the mmap path. Keys
// are taken exactly as the file stores them, whatever its KeyFormat.
//
//   build/storeProbe [--spec 740210A] 0x1234... 0xabcd...
//
// Without --spec it reads the file solver/singleHand/config.h names.

#include "server/strategyStore.h"

#include <cstdlib>

int main(int argc, char** argv) {
    try {
        std::string spec = StrategyConfig{}.spec();
        int first = 1;
        if (argc > 2 && std::string(argv[1]) == "--spec") {
            spec = argv[2];
            first = 3;
        }
        StrategyStore store;
        store.open(strategyPath(spec), indexPathFor(strategyPath(spec)));
        std::cout << std::setprecision(17);
        for (int i = first; i < argc; i++) {
            uint64_t key = std::strtoull(argv[i], nullptr, 0);
            StrategyRecord record;
            if (!store.lookupFileKey(key, record)) {
                std::cout << key << " MISS" << std::endl;
                continue;
            }
            std::cout << key;
            for (int j = 0; j < 4; j++) {
                std::cout << " " << record.weights[j];
            }
            for (int j = 0; j < 4; j++) {
                std::cout << " " << decodeVariance(record.variances, j);
            }
            std::cout << std::endl;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
