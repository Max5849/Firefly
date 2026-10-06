#include "solver/config.h"
#include "solver/engine.h"

double MIN_DELTA_INC;
double DELTA_SCALAR;
double INV_MIN_DELTA_INC;
double INV_DELTA_SCALAR;

struct Explorer {

    struct EntryBase {
        std::array<uint64_t, 4> weights;
    };

    struct Entry {
        std::array<uint64_t, 4> weights;
        uint64_t visits : 40;
        uint64_t delta : 16;
        uint64_t options : 8;
    };

    struct EpochNode {
        std::array<Entry*, 1326> entries;
        int option;
        int action;
        float chips;
        float pot;
    };

    inline static ankerl::unordered_dense::map<uint64_t, Entry> strategy;
    inline static ankerl::unordered_dense::map<uint64_t, EntryBase> strategyBase;
    std::mt19937 gen;
    std::uniform_real_distribution<double> drawUniform;
    std::vector<EpochNode> epochPath;

    Explorer() : gen(15) {}

    int chooseOption(std::array<uint64_t, 1326>& keys, std::array<uint64_t, 1326>& keysBase, std::array<float, 1326>& range, 
            std::array<bool, 1326>& commonRange, float pot, std::array<bool, 5> options, int action, float chips) {
        epochPath.emplace_back();
        EpochNode& epochNode = epochPath.back();
        epochNode.action = action;
        epochNode.chips = chips;
        epochNode.pot = pot;
        std::array<double, 5> drawTable = {0};
        std::array<std::array<double, 5>, 1326> drawTableIndividual = {0};
        for (int i = 0; i < 1326; i++) {
            if (!commonRange[i]) {
                continue;
            }
            auto [it, inserted] = strategy.try_emplace(keys[i]);
            Entry& entry = it->second;
            if (inserted) {
                uint8_t legal = 0;
                for (int j = 0; j < 5; j++) {
                    if (options[j]) {
                        legal |= uint8_t(1u << j);
                    }
                }
                if (USE_BASE) {
                    auto it = strategyBase.find(keysBase[i]);
                    if (it != strategyBase.end()) {
                        EntryBase& entryBase = it->second;
                        for (int j = 0; j < 4; j++) {
                            entry.weights[j] = entryBase.weights[i];
                        }
                        std::array<double, 5> drawTableBase = {0};
                        double sum = 0;
                        if (legal & (1u)) {
                            drawTableBase[0] += 1.0;
                            sum += drawTableBase[0];
                        }
                        for (int j = 1; j < 5; j++) {
                            if (legal & (1u << j)) {
                                drawTableBase[j] += std::exp(INV_TEMP * entry.weights[j - 1]);
                                sum += drawTableBase[j];
                            }
                        }
                        for (int j = 0; j < 5; j++) {
                            if (BASE_CONFIDENCE * drawTableBase[j] / sum < 1.0) {
                                legal &= ~(1u << j);
                            }
                        }
                    }
                }
                entry.delta = 65535;
                entry.options = legal;
            }
            if (entry.options & (1u)) {
                drawTable[0] += range[i] * 1.0;
                drawTableIndividual[i][0] += 1.0;
            }
            for (int j = 1; j < 5; j++) {
                if (entry.options & (1u << j)) {
                    drawTable[j] += range[i] * std::exp(INV_TEMP * entry.weights[j - 1]);
                    drawTableIndividual[i][j] += std::exp(INV_TEMP * entry.weights[j - 1]);
                }
            }
            epochNode.entries[i] = &entry;
            entry.visits++;
        }
        for (int i = 1; i < 5; i++) {
            drawTable[i] += drawTable[i - 1];
        }
        double draw = drawTable[4] * drawUniform(gen);
        int option = 0;
        for (int i = 0; i < 5; i++) {
            if (draw < drawTable[i]) {
                option = i;
                break;
            }
        }
        epochNode.option = option;
        for (int i = 0; i < 1326; i++) {
            if (!commonRange[i]) {
                continue;
            }
            double sum = 0;
            for (int j = 0; j < 5; j++) {
                sum += drawTableIndividual[i][j];
            }
            range[i] *= drawTableIndividual[i][option] / sum;
            
        }
        return option;
    }

    void updateWeights(std::array<std::array<float, 1326>, PLAYER_COUNT> rewards, 
            std::array<bool, 1326>& commonRange, double strength) {
        for (auto& epochNode : epochPath) {
            for (int i = 0; i < 1326; i++) {
                if (!commonRange[i]) {
                    continue;
                }
                Entry* entry = epochNode.entries[i];
                float target = (rewards[epochNode.action][i] - epochNode.chips) / epochNode.pot;
                double scalar = strength * MIN_DELTA_INC * std::exp(DELTA_SCALAR * entry->delta);
                for (int j = 1; j < 5; j++) {
                    float delta = target - entry->weights[j - 1];
                    target += scalar * delta;
                }
            }
        }
    }

    void eraseNoise() {
        for (auto& [key, entry] : strategy) {
            uint64_t visits = entry.visits;
            if (visits < 5) {
                strategy.erase(key);
            }
        }
    }

    void updateDeltas() {
        for (auto& [key, entry] : strategy) {
            uint64_t visits = entry.visits;
            double alpha = -std::expm1(-1.0 / std::pow(std::max(double(visits), 500.0), FLEXIBILITY));
            entry.delta = std::lround(std::log(alpha * INV_MIN_DELTA_INC) * INV_DELTA_SCALAR);
            entry.visits = 0;
        }
    }

    static std::string spec(bool metasBasic, bool memoryBasic) {
        return std::to_string((int)(10000 * PLAYER_COUNT + 10 * CHIP_COUNT
            + 10 * metasBasic + memoryBasic));
    }
    
    static void downloadStrategyBase() {
        static std::once_flag loadFlag;
        std::call_once(loadFlag, [] {
            std::ifstream readFile("solver/strategies/strategy_"
                + spec(BASE_METAS_BASIC, BASE_MEMORY_BASIC) + ".bin", std::ios::binary);
            uint64_t size = 0;
            readFile.read(reinterpret_cast<char*>(&size), sizeof(size));
            strategyBase.reserve(size);  
            for (uint64_t i = 0; i < size; i++) {
                uint64_t key = 0;
                std::array<float, 4> in;
                uint64_t visits = 0;
                readFile.read(reinterpret_cast<char*>(&key), sizeof(key));
                readFile.read(reinterpret_cast<char*>(in.data()), sizeof(in));
                readFile.read(reinterpret_cast<char*>(&visits), sizeof(visits));
                auto [it, inserted] = strategyBase.try_emplace(key);
                EntryBase& entryBase = it->second;
                for (int j = 0; j < 4; j++) {
                    entryBase.weights[j] = in[j];
                }
            }
            readFile.close();
        });
    }

    void uploadStrategy() {
        std::ofstream writeFile("solver/strategies/strategy_"
            + spec(METAS_BASIC, MEMORY_BASIC) + ".bin");
        uint64_t size = strategy.size();
        writeFile.write(reinterpret_cast<const char*>(&size), sizeof(size));
        for (auto& [key, entry] : strategy) {
            std::array<float, 4> out;
            for (int i = 0; i < 4; i++) {
                out[i] = float(entry.weights[i]);
            }
            uint64_t visits = entry.visits;
            writeFile.write(reinterpret_cast<const char*>(&key), sizeof(key));
            writeFile.write(reinterpret_cast<const char*>(out.data()), sizeof(out));
            writeFile.write(reinterpret_cast<const char*>(&visits), sizeof(visits));
        }
        writeFile.close();
    }
};

void playHand(Engine& engine, Explorer& explorer, bool updateWeights, double strength) {
    engine.shuffle(explorer.gen);
    engine.deal();
    int option;
    do {
        std::array<uint64_t, 1326> keys = engine.getState(MEMORY_BASIC, METAS_BASIC);
        std::array<uint64_t, 1326> keysBase;
        if (USE_BASE) {
            keysBase = engine.getState(BASE_MEMORY_BASIC, BASE_METAS_BASIC);
        } else {
            keysBase = {0};
        }
        option = explorer.chooseOption(keys, keysBase, engine.getRange(), engine.getCommonRange(), 
            engine.getPot(), engine.getOptions(), engine.getAction(), engine.getChips());
    } while (engine.play(option));
    if (updateWeights) {
        explorer.updateWeights(engine.getRewards(), engine.getCommonRange(), strength);
    }
}

int main () {
    try {
        MIN_DELTA_INC = -std::expm1(-1.0 / std::pow(2, 40 * FLEXIBILITY));
        INV_MIN_DELTA_INC = 1.0 / MIN_DELTA_INC;
        DELTA_SCALAR = std::log(-std::expm1(-1.0 / 500.0) * INV_MIN_DELTA_INC) / 65536.0;
        INV_DELTA_SCALAR = 1.0 / DELTA_SCALAR;
        Engine::loadTables();
        if (USE_BASE) {
            Explorer::downloadStrategyBase();
        }
        Engine engine;
        Explorer explorer;

        for (int i = 0; i < EPOCHS / 20; i++) {
            playHand(engine, explorer, false, 1.0);
        }
        explorer.eraseNoise();
        explorer.updateDeltas();
        for (int block = 0; block < 20; block++) {
            for (int i = 0; i < EPOCHS / 20; i++) {
                playHand(engine, explorer, true, 1.0);
            }
            explorer.eraseNoise();
            explorer.updateDeltas();
        }
        for (int i = 0; i < EPOCHS / 20; i++) {
            playHand(engine, explorer, true, 1.0 - i / double(EPOCHS / 20));
        }
        explorer.eraseNoise();
        explorer.uploadStrategy();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
