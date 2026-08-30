#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"

bool mergeClusterPair(std::vector<std::vector<uint64_t>>& clusterHands, std::vector<uint64_t>& clusterSizes,
        std::vector<std::array<float, 110>>& clusterPoints, int begin, int end, int mergeCount, int maxClusters) {
    double minDistance = std::numeric_limits<double>::max();
    int a;
    int b;
    for (int i = begin; i < end - mergeCount - 1; i++) {
        for (int q = i + 1; q < end - mergeCount; q++) {
            double distance = 0;
            double aSum = 0;
            double bSum = 0;
            for (int p = 0; p < 110; p++) {
                aSum += clusterPoints[i][p];
                bSum += clusterPoints[q][p];
                distance += (109.5 - p) * (109.5 - p) * std::abs(aSum - bSum);
            }
            if (distance < minDistance) {
                minDistance = distance;
                a = i;
                b = q;
            }
        }
    }
    if (end - begin - mergeCount <= maxClusters) {
        return false;
    }
    for (int i = 0; i < 110; i++) { 
        clusterPoints[a][i] = (clusterPoints[a][i] * clusterSizes[a] + clusterPoints[b][i]
            * clusterSizes[b]) / (clusterSizes[a] + clusterSizes[b]);
    }
    clusterHands[a].insert(clusterHands[a].end(), clusterHands[b].begin(), clusterHands[b].end());
    clusterSizes[a] += clusterSizes[b];
    clusterSizes.erase(clusterSizes.begin() + b);
    clusterHands.erase(clusterHands.begin() + b);
    clusterPoints.erase(clusterPoints.begin() + b);
    return true;
}

void clusterDists(std::unordered_map<uint64_t, std::array<float, 110>>& handDists, 
        std::unordered_map<uint64_t, uint8_t>& handMetasBasic,
        std::unordered_map<uint64_t, uint8_t>& handMetas, int maxClusters) {
    std::vector<std::vector<uint64_t>> clusterHands;
    std::vector<uint64_t> clusterSizes;
    std::vector<std::array<float, 110>> clusterPoints;
    std::vector<uint64_t> keys;
    keys.reserve(handDists.size());
    for (auto& q : handDists) {
        keys.push_back(q.first);
    }
    std::sort(keys.begin(), keys.end(), [](uint64_t x, uint64_t y) {
        uint32_t bx = static_cast<uint32_t>(x), by = static_cast<uint32_t>(y);
        return bx != by ? bx < by : x < y;
    });
    for (uint64_t key : keys) {
        std::array<float, 110>& dist = handDists[key];
        clusterHands.push_back({key});
        float sum = 0.0f;
        for (int i = 0; i < 110; i++) {
            sum += dist[i];
        }
        for (int i = 0; i < 110; i++) {
            dist[i] /= sum;
        }
        clusterSizes.push_back(sum);
        clusterPoints.push_back(dist);
    }
    std::vector<int> divisions;
    for (size_t i = 0; i < keys.size();) {
        divisions.push_back(static_cast<int>(i));
        uint32_t boardClass = static_cast<uint32_t>(keys[i]);
        while (i < keys.size() && static_cast<uint32_t>(keys[i]) == boardClass) {
            i++;
        }
    }
    divisions.push_back(static_cast<int>(keys.size()));
    for (int i = divisions.size() - 2; i >= 0; i--) {
        int mergeCount = 0;
        while (mergeClusterPair(clusterHands, clusterSizes, clusterPoints,
                divisions[i], divisions[i + 1], mergeCount, maxClusters)) {
            mergeCount++;
        }
        for (int j = divisions[i]; j < divisions[i + 1] - mergeCount; j++) {
            for (auto& q : clusterHands[j]) {
                handMetas[q] = j - divisions[i];
            }
        }
    }
    int mergeCount = 0;
    int clusterHandsSize = clusterHands.size();
    while (mergeClusterPair(clusterHands, clusterSizes, clusterPoints, 
            0, clusterHandsSize, mergeCount, maxClusters)) {
        mergeCount++;
    }
    for (int i = 0; i < clusterHands.size(); i++) {
        for (auto& q : clusterHands[i]) {
            handMetasBasic[q] = i;
        }
    }
}

int main() {
    Dealer dealer;
    std::vector<uint16_t> boardClasses;
    boardClasses.resize(60929700);
    std::ifstream readFile("handAnalysis/boardClasses.bin", std::ios::binary);
    readFile.read(reinterpret_cast<char*>(boardClasses.data()), boardClasses.size() * sizeof(uint16_t));
    std::unordered_map<uint64_t, uint8_t> handMetasBasic;
    std::unordered_map<uint64_t, uint8_t> handMetas;
    std::set<uint32_t> flopClasses;
    while (dealer.flopGen()) {
        flopClasses.insert(boardClasses[dealer.getBoardIndex(1)]);
        dealer.flopSet();
    }

    std::vector<uint32_t> flopClassList(flopClasses.begin(), flopClasses.end());
    std::mutex metaMutex;
    std::atomic<size_t> nextIndex{0};
    std::atomic<int> done{0};
    unsigned numThreads = std::max(1u, std::thread::hardware_concurrency());

    auto worker = [&]() {
        Dealer dealer;
        HandClusterer handClusterer;
        while (true) {
            size_t ci = nextIndex.fetch_add(1);
            if (ci >= flopClassList.size()) break;
            uint32_t u = flopClassList[ci];

            std::unordered_map<uint64_t, std::array<float, 110>> flopHandDists;
            std::unordered_map<uint64_t, std::array<float, 110>> turnHandDists;
            std::unordered_map<uint64_t, std::array<float, 110>> riverHandDists;
            dealer = Dealer();
            while (dealer.flopGen()) {
                uint32_t flopClass = boardClasses[dealer.getBoardIndex(1)];
                if (flopClass != u) {
                    dealer.flopSet();
                    continue;
                }
                std::array<uint32_t, 1326> flopHandClasses;
                bool onFlop[52] = {false};
                for (int k = 0; k < 3; k++) onFlop[dealer.boardCards[k]] = true;
                for (int i = 0; i < 51; i++) {
                    if (onFlop[i]) continue;
                    for (int j = i + 1; j < 52; j++) {
                        if (onFlop[j]) continue;
                        uint32_t handClass = handClusterer.classifyHand(dealer.boardCards, {i, j}, 1);
                        flopHandClasses[i * (101 - i) / 2 + j - 1] = handClass;
                    }
                }
                while (dealer.turnGen()) {
                    uint32_t turnClass = boardClasses[dealer.getBoardIndex(2)];
                    std::array<uint32_t, 1326> turnHandClasses;
                    bool onTurn[52] = {false};
                    for (int k = 0; k < 4; k++) onTurn[dealer.boardCards[k]] = true;
                    for (int i = 0; i < 51; i++) {
                        if (onTurn[i]) continue;
                        for (int j = i + 1; j < 52; j++) {
                            if (onTurn[j]) continue;
                            uint32_t handClass = handClusterer.classifyHand(dealer.boardCards, {i, j}, 2);
                            turnHandClasses[i * (101 - i) / 2 + j - 1] = handClass;
                        }
                    }
                    while (dealer.riverGen()) {
                        uint32_t riverClass = boardClasses[dealer.getBoardIndex(3)];
                        std::array<uint32_t, 1326> riverHandClasses;
                        bool onRiver[52] = {false};
                        for (int k = 0; k < 5; k++) onRiver[dealer.boardCards[k]] = true;
                        for (int i = 0; i < 51; i++) {
                            if (onRiver[i]) continue;
                            for (int j = i + 1; j < 52; j++) {
                                if (onRiver[j]) continue;
                                uint32_t handClass = handClusterer.classifyHand(dealer.boardCards, {i, j}, 3);
                                riverHandClasses[i * (101 - i) / 2 + j - 1] = handClass;
                            }
                        }
                        std::array<int, 1326> referenceRanks = dealer.rankHands();
                        for (int i = 0; i < 1326; i++) {
                            int referenceRank = referenceRanks[i];
                            if (referenceRanks[i] == -1) {
                                continue;
                            }
                            flopHandDists[((uint64_t)flopClass) | ((uint64_t)flopHandClasses[i] << 32)][referenceRank]++;
                            turnHandDists[((uint64_t)turnClass) | ((uint64_t)turnHandClasses[i] << 32)][referenceRank]++;
                            riverHandDists[((uint64_t)riverClass) | ((uint64_t)riverHandClasses[i] << 32)][referenceRank]++;
                        }
                        dealer.riverSet();
                    }
                    dealer.turnSet();
                }
                dealer.flopSet();
            }
            std::unordered_map<uint64_t, uint8_t> localMetasBasic;
            std::unordered_map<uint64_t, uint8_t> localMetas;
            clusterDists(flopHandDists, localMetasBasic, localMetas, 32);
            clusterDists(turnHandDists, localMetasBasic, localMetas, 32);
            clusterDists(riverHandDists, localMetasBasic, localMetas, 16);
            std::lock_guard<std::mutex> lock(metaMutex);
            for (auto& [key, value] : localMetasBasic) {
                handMetasBasic[key] = value;
            }
            for (auto& [key, value] : localMetas) {
                handMetas[key] = value;
            }
            std::cout << ++done << " / " << flopClassList.size() << " flop classes processed\n";
        }
    };

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < numThreads; t++) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
    std::ofstream fileBasic("handMetasBasic.bin", std::ios::binary);
    uint64_t size = handMetasBasic.size();
    fileBasic.write(reinterpret_cast<const char*>(&size), sizeof(size));
    for (const auto& [key, value] : handMetasBasic) {
        fileBasic.write(reinterpret_cast<const char*>(&key), sizeof(key));
        fileBasic.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    fileBasic.close();
    
    std::ofstream file("handMetas.bin", std::ios::binary);
    size = handMetas.size();
    file.write(reinterpret_cast<const char*>(&size), sizeof(size));
    for (const auto& [key, value] : handMetas) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key));
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    file.close();
    return 0;
}
