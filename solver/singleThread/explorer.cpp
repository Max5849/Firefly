#include "solver/singleThread/config.h"
#include "solver/singleThread/engine.h"

double MIN_DELTA_INC;
double DELTA_SCALAR;
double INV_MIN_DELTA_INC;
double INV_DELTA_SCALAR;
uint64_t EPOCH_BLOCK;

struct Explorer {

    struct EntryBase {
        std::array<double, 4> weights;
    };

    struct Entry {
        std::array<double, 4> weights;
        uint32_t visits;
        uint8_t visitsFraction;
        uint16_t delta;
        uint8_t options;
    };

    struct EpochNode {
        std::array<uint64_t, 1326> keys;
        std::array<float, 1326> reach;
        double odds;
        int option;
        int action;
        float chips;
        float pot;
    };

    inline static ankerl::unordered_dense::map<uint64_t, Entry> strategy;
    inline static ankerl::unordered_dense::map<uint64_t, EntryBase> strategyBase;
    Engine engine;
    std::mt19937 gen;
    std::uniform_real_distribution<double> drawUniform;
    std::vector<EpochNode> epochPath;

    Explorer() : gen(15) {}

    void runPhase(int phase) {
        if (phase == 0) {
            for (int i = 0; i < EPOCH_BLOCK; i++) {
                playHand(true, false, i);
            }
            eraseNoise();
            updateDeltas();
        } else if (phase == 1) {
            for (int i = 0; i < 20; i++) {
                for (int j = 0; j < EPOCH_BLOCK; j++) {
                    playHand(false, false, j);
                }
                eraseNoise();
                updateDeltas();
            }
        } else if (phase == 2) {
            for (int i = 0; i < EPOCH_BLOCK; i++) {
                playHand(false, true, i);
            }
            eraseNoise();
        }
    }

    void playHand(bool warmUp, bool coolDown, uint64_t blockEpoch) {
        engine.shuffle(gen);
        engine.deal();
        while (true) {
            do {
                if (!MEMORY_BASIC || !BASE_MEMORY_BASIC) {
                    engine.setActionState(false);
                }
                if (MEMORY_BASIC || BASE_MEMORY_BASIC) {
                    engine.setActionState(true);
                }
            } while (engine.play(chooseOption()));
            if (!engine.beginNewRound()) {
                engine.setRewards();
                break;
            }
            engine.setCommonRange();
            if (!METAS_BASIC || !BASE_METAS_BASIC) {
                engine.setMetaState(false);
            }
            if (METAS_BASIC || BASE_METAS_BASIC) {
                engine.setMetaState(true);
            }
        }
        if (warmUp) {
            recordVisits();
            epochPath.clear();
        } else if (coolDown) {
            recordVisits();
            updateWeights(1.0 - blockEpoch / double(EPOCH_BLOCK));
            epochPath.clear();
        } else {
            recordVisits();
            updateWeights(1.0);
            epochPath.clear();
        }
    }

    int chooseOption() {
        epochPath.emplace_back();
        EpochNode& epochNode = epochPath.back();
        std::array<bool, 5> options = engine.getOptions();
        epochNode.action = engine.getAction();
        epochNode.chips = engine.getChips();
        epochNode.pot = engine.getPot();
        std::array<bool, 1326>& commonRange = engine.getCommonRange();
        std::array<float, 1326>& range = engine.getRange(epochNode.action);
        std::array<uint64_t, 1326>& metaState = engine.getMetaState(METAS_BASIC);
        uint64_t actionState = engine.getActionState(MEMORY_BASIC);
        std::array<uint64_t, 1326>& metaStateBase = engine.getMetaState(BASE_METAS_BASIC);; 
        uint64_t actionStateBase = engine.getActionState(BASE_MEMORY_BASIC);
        
        std::array<std::array<double, 5>, 1326> drawTable = {0};
        std::array<double, 1326> drawTableSums = {0};
        std::array<std::array<double, 5>, 1326> sampleTable = {0};
        std::array<double, 1326> sampleTableSums = {0};
        epochNode.odds = engine.getOdds(epochNode.action);
        for (int i = 0; i < 1326; i++) {
            epochNode.reach[i] = commonRange[i] ? range[i] : 0.0f;
            if (!commonRange[i] || !(range[i] > 0.0f)) {
                continue;
            }
            uint64_t key = actionState | metaState[i] << 42;
            auto [it, inserted] = strategy.try_emplace(key);
            Entry& entry = it->second;
            if (inserted) {
                uint8_t legal = 0;
                for (int j = 0; j < 5; j++) {
                    if (options[j]) {
                        legal |= uint8_t(1u << j);
                    }
                }
                if (USE_BASE) {
                    uint64_t keyBase = actionStateBase | metaStateBase[i] << 42;
                    auto it = strategyBase.find(keyBase);
                    if (it != strategyBase.end()) {
                        EntryBase& entryBase = it->second;
                        for (int j = 0; j < 4; j++) {
                            entry.weights[j] = entryBase.weights[j];
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
                drawTable[i][0] += 1.0;
                drawTableSums[i] += 1.0;
                sampleTable[i][0] += 1.0;
                sampleTableSums[i] += 1.0;
            }
            for (int j = 1; j < 5; j++) {
                if (entry.options & (1u << j)) {
                    drawTable[i][j] += std::exp(std::clamp(INV_TEMP * entry.weights[j - 1], -700.0, 700.0));
                    drawTableSums[i] += drawTable[i][j];
                    sampleTable[i][j] += std::exp(std::clamp(SAMPLE_INV_TEMP * entry.weights[j - 1], -700.0, 700.0));
                    sampleTableSums[i] += sampleTable[i][j];
                }
            }
            epochNode.keys[i] = key;
        }
        std::array<double, 5> drawShares = {0};
        std::array<double, 5> sampleShares = {0};
        for (int i = 0; i < 1326; i++) {
            if (!commonRange[i] || !(range[i] > 0.0f)) {
                continue;
            }
            for (int j = 0; j < 5; j++) {
                drawShares[j] += range[i] * drawTable[i][j] / drawTableSums[i];
                sampleShares[j] += range[i] * sampleTable[i][j] / sampleTableSums[i];
            }
        }
        std::array<double, 5> drawTableCompressed = sampleShares;
        for (int i = 1; i < 5; i++) {
            drawTableCompressed[i] += drawTableCompressed[i - 1];
        }
        double draw = drawTableCompressed[4] * drawUniform(gen);
        int option = 0;
        for (int i = 0; i < 5; i++) {
            if (draw < drawTableCompressed[i] && sampleShares[i] > 0) {
                option = i;
                break;
            }
        }
        epochNode.option = option;
        double sampleShare = sampleShares[option] / drawTableCompressed[4];
        double targetShare = drawShares[option] / drawTableCompressed[4];
        engine.getOdds(epochNode.action) *= sampleShare;
        for (int p = 0; p < PLAYER_COUNT; p++) {
            if (p != epochNode.action && engine.players[p].inHand) {
                engine.getOdds(p) *= sampleShare / targetShare;
            }
        }
        double rangeSum = 0;
        for (int i = 0; i < 1326; i++) {
            if (!commonRange[i] || !(range[i] > 0.0f)) {
                continue;
            }
            range[i] *= drawTable[i][option] / drawTableSums[i];
            rangeSum += range[i];
        }
        for (int i = 0; i < 1326; i++) {
            if (commonRange[i] && range[i] < 0.00003771 * rangeSum) {
                range[i] = 0.0f;
            }
        }
        return option;
    }

    void recordVisits() {
        for (auto& epochNode : epochPath) {
            for (int i = 0; i < 1326; i++) {
                if (!(epochNode.reach[i] > 0.0f)) {
                    continue;
                }
                Entry& entry = strategy.at(epochNode.keys[i]);
                float upscaledRange = float(256.0 * epochNode.reach[i] / epochNode.odds);
                uint32_t add = uint32_t(upscaledRange);
                add += drawUniform(gen) < (upscaledRange - add);
                uint32_t overflowFraction = entry.visitsFraction + add;
                uint64_t visits = uint64_t(entry.visits) + (overflowFraction >> 8);
                entry.visits = uint32_t(std::min(visits, uint64_t(UINT32_MAX)));
                entry.visitsFraction = uint8_t(overflowFraction);
            }
        }
    }

    void eraseNoise() {
        std::erase_if(strategy, [&](const auto& entry) { 
            return entry.second.visits < 5; 
        });
    }

    void updateDeltas() {
        for (auto& [key, entry] : strategy) {
            double visits = std::clamp(double(entry.visits), VISITS_FLOOR, VISITS_CEIL);
            double rate = 1.0 / std::pow(visits, FLEXIBILITY);
            long encoded = std::lround(std::log(rate * INV_MIN_DELTA_INC) * INV_DELTA_SCALAR);
            entry.delta = uint16_t(std::clamp(encoded, 0L, 65535L));
            entry.visits = 0;
            entry.visitsFraction = 0;
        }
    }

    void updateWeights(double strength) {
        std::array<bool, 1326>& commonRange = engine.getCommonRange();
        const bool showdown = engine.heads > 1;
        for (auto& epochNode : epochPath) {
            if (epochNode.option == 0) {
                continue;
            }
            std::array<float, 1326>& range = engine.getRange(epochNode.action);
            std::array<float, 1326>& reward = engine.getReward(epochNode.action);
            const double odds = engine.getOdds(epochNode.action);
            for (int i = 0; i < 1326; i++) {
                if (!commonRange[i] || !(range[i] > 0.0f) || (showdown && engine.comboConflicts(i, 5))) {
                    continue;
                }
                Entry& entry = strategy.at(epochNode.keys[i]);
                double target = (reward[i] - epochNode.chips) / epochNode.pot;
                double scalar = -std::expm1(-LEARNING_RATE * strength * range[i] / odds
                    * MIN_DELTA_INC * std::exp(DELTA_SCALAR * entry.delta));
                double& weight = entry.weights[epochNode.option - 1];
                weight += scalar * (target - weight);
            }
        }
    }

    static std::string spec(bool metasBasic, bool memoryBasic, bool ranged) {
        return std::to_string((int)(100000 * PLAYER_COUNT + 100 * CHIP_COUNT
            + 100 * metasBasic + 10 * memoryBasic + ranged));
    }
    
    static void downloadStrategyBase() {
        static std::once_flag loadFlag;
        std::call_once(loadFlag, [] {
            std::ifstream readFile("solver/strategies/strategy_"
                + spec(BASE_METAS_BASIC, BASE_MEMORY_BASIC, BASE_RANGED) + ".bin", std::ios::binary);
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
            + spec(METAS_BASIC, MEMORY_BASIC, RANGED) + ".bin");
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

int main () {
    try {
        MIN_DELTA_INC = 1.0 / std::pow(VISITS_CEIL, FLEXIBILITY);
        INV_MIN_DELTA_INC = 1.0 / MIN_DELTA_INC;
        DELTA_SCALAR = std::log(1.0 / std::pow(VISITS_FLOOR, FLEXIBILITY) * INV_MIN_DELTA_INC) / 65535.0;
        INV_DELTA_SCALAR = 1.0 / DELTA_SCALAR;
        EPOCH_BLOCK = EPOCHS / 20;
        Engine::loadTables();
        if (USE_BASE) {
            Explorer::downloadStrategyBase();
        }
        Explorer explorer;

        explorer.runPhase(0);
        explorer.runPhase(1);
        explorer.runPhase(2);

        explorer.uploadStrategy();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
