// Builds the sidecar hash index for a strategy file:
//
//   build/indexBuilder [strategy.bin]
//   build/indexBuilder --players 7 --chips 400 --ante 2 --metas 1 --memory 0 --tag A
//
// With no arguments it indexes the file solver/singleHand/config.h names. The server builds a missing or
// stale index itself on first use; this does it ahead of time, and checks the result.

#include "server/strategyStore.h"

int main(int argc, char** argv) {
    try {
        StrategyConfig config;
        std::string path;
        for (int i = 1; i < argc; i++) {
            std::string flag = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error(flag + " needs a value");
                }
                return argv[++i];
            };
            if (flag == "--players") {
                config.players = std::stoi(next());
            } else if (flag == "--chips") {
                config.chips = std::stoi(next());
            } else if (flag == "--ante") {
                config.ante = std::stoi(next());
            } else if (flag == "--metas") {
                config.metasStage = std::stoi(next());
            } else if (flag == "--memory") {
                config.memoryStage = std::stoi(next());
            } else if (flag == "--tag") {
                const std::string tag = next();
                if (tag.size() != 1 || !validTag(tag[0])) {
                    throw std::runtime_error("--tag takes a single letter");
                }
                config.tag = tag[0];
            } else if (path.empty() && flag.rfind("--", 0) != 0) {
                path = flag;
            } else {
                throw std::runtime_error("unrecognised argument " + flag);
            }
        }
        if (path.empty()) {
            path = strategyPath(config.spec());
        }
        buildIndex(path, indexPathFor(path), &std::cout);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
