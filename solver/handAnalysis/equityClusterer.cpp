#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"

bool BASIC = false;

void computeCdf(const std::array<float, 165>& point, std::array<double, 165>& cdf) {
    double sum = 0;
    for (int p = 0; p < 165; p++) {
        sum += point[p];
        cdf[p] = sum;
    }
}

double cdfDistance(const std::array<double, 165>& a, const std::array<double, 165>& b) {
    double distance = 0;
    for (int p = 0; p < 165; p++) {
        distance += (164.5 - p) * (164.5 - p) * std::abs(a[p] - b[p]);
    }
    return distance;
}

float distStrength(const std::array<float, 165>& point) {
    double sum = 0;
    double strength = 0;
    for (int p = 0; p < 165; p++) {
        sum += point[p];
        strength += (164.5 - p) * sum;
    }
    return float(strength / (165 * 164.5 - 164 * 165 / 2));
}

void mergeDivision(std::vector<std::vector<uint64_t>>& clusterHands, std::vector<uint64_t>& clusterSizes,
        std::vector<std::array<float, 165>>& clusterPoints, std::vector<char>& alive,
        int begin, int end, int maxClusters) {
    int n = end - begin;
    std::vector<std::array<double, 165>> cdfs(n);
    std::vector<int> nn(n);
    std::vector<double> nnDist(n);
    std::vector<double> strengths(n);
    for (int i = 0; i < n; i++) {
        computeCdf(clusterPoints[begin + i], cdfs[i]);
        strengths[i] = distStrength(clusterPoints[begin + i]);
    }
    // Ward's merge cost: the cdf distance scaled by na * nb / (na + nb), so merging two
    // large clusters costs more than folding a small one into a large one, and by
    // sqrt(sa^2 + sb^2) over distStrength, so any merge involving a strong cluster costs more.
    auto linkage = [&](int i, int j) {
        double na = clusterSizes[begin + i];
        double nb = clusterSizes[begin + j];
        double sa = strengths[i];
        double sb = strengths[j];
        return std::sqrt(na * nb / (na + nb)) * (sa * sa + sb * sb) * cdfDistance(cdfs[i], cdfs[j]);
    };
    auto rescan = [&](int i) {
        nn[i] = -1;
        nnDist[i] = std::numeric_limits<double>::max();
        for (int j = i + 1; j < n; j++) {
            if (!alive[begin + j]) continue;
            double distance = linkage(i, j);
            if (distance < nnDist[i]) {
                nnDist[i] = distance;
                nn[i] = j;
            }
        }
    };
    for (int i = 0; i < n; i++) {
        rescan(i);
    }
    for (int live = n; live > maxClusters; live--) {
        int a = -1;
        double minDistance = std::numeric_limits<double>::max();
        for (int i = 0; i < n; i++) {
            if (alive[begin + i] && nnDist[i] < minDistance) {
                minDistance = nnDist[i];
                a = i;
            }
        }
        int b = nn[a];
        int ga = begin + a;
        int gb = begin + b;
        for (int i = 0; i < 165; i++) {
            clusterPoints[ga][i] = (clusterPoints[ga][i] * clusterSizes[ga] + clusterPoints[gb][i]
                * clusterSizes[gb]) / (clusterSizes[ga] + clusterSizes[gb]);
        }
        clusterHands[ga].insert(clusterHands[ga].end(), clusterHands[gb].begin(), clusterHands[gb].end());
        clusterSizes[ga] += clusterSizes[gb];
        alive[gb] = 0;
        computeCdf(clusterPoints[ga], cdfs[a]);
        strengths[a] = distStrength(clusterPoints[ga]);
        rescan(a);
        // Only clusters below b can have pointed at a or b; those below a may now prefer the new a.
        for (int i = 0; i < b; i++) {
            if (!alive[begin + i] || i == a) continue;
            if (nn[i] == a || nn[i] == b) {
                rescan(i);
            } else if (i < a) {
                double distance = linkage(i, a);
                if (distance < nnDist[i] || (distance == nnDist[i] && a < nn[i])) {
                    nnDist[i] = distance;
                    nn[i] = a;
                }
            }
        }
    }
}

void clusterDists(std::unordered_map<uint64_t, std::array<float, 165>>& handDists, 
        std::unordered_map<uint64_t, uint8_t>& handMetas, int maxClusters) {
    std::vector<std::vector<uint64_t>> clusterHands;
    std::vector<uint64_t> clusterSizes;
    std::vector<std::array<float, 165>> clusterPoints;
    std::vector<uint64_t> keys;
    keys.reserve(handDists.size());
    for (auto& q : handDists) {
        keys.push_back(q.first);
    }
    std::sort(keys.begin(), keys.end(), [](uint64_t x, uint64_t y) {
        uint16_t bx = static_cast<uint16_t>(x), by = static_cast<uint16_t>(y);
        return bx != by ? bx < by : x < y;
    });
    for (uint64_t key : keys) {
        std::array<float, 165>& dist = handDists[key];
        clusterHands.push_back({key});
        float sum = 0.0f;
        for (int i = 0; i < 165; i++) {
            sum += dist[i];
        }
        for (int i = 0; i < 165; i++) {
            dist[i] /= sum;
        }
        clusterSizes.push_back(sum);
        clusterPoints.push_back(dist);
    }
    std::vector<int> divisions;
    for (size_t i = 0; i < keys.size();) {
        divisions.push_back(static_cast<int>(i));
        uint32_t boardClass = static_cast<uint16_t>(keys[i]);
        while (i < keys.size() && static_cast<uint16_t>(keys[i]) == boardClass) {
            i++;
        }
    }
    divisions.push_back(static_cast<int>(keys.size()));
    std::vector<char> alive(keys.size(), 1);
    for (int i = divisions.size() - 2; i >= 0; i--) {
        mergeDivision(clusterHands, clusterSizes, clusterPoints, alive, divisions[i], divisions[i + 1], maxClusters);
        int cluster = 0;
        for (int j = divisions[i]; j < divisions[i + 1]; j++) {
            if (!alive[j]) continue;
            for (auto& q : clusterHands[j]) {
                handMetas[uint64_t(q) & ~uint64_t(0xFFFF)] = cluster;
            }
            cluster++;
        }
    }
}

int main() {
    Dealer dealer;
    std::vector<uint16_t> boardClasses;
    boardClasses.resize(60929700);
    std::ifstream readFile("solver/handAnalysis/boardClasses.bin", std::ios::binary);
    readFile.read(reinterpret_cast<char*>(boardClasses.data()), boardClasses.size() * sizeof(uint16_t));
    std::vector<uint16_t> boardClassesBasic;
    boardClassesBasic.resize(60929700);
    std::ifstream otherFile("solver/handAnalysis/boardClassesBasic.bin", std::ios::binary);
    otherFile.read(reinterpret_cast<char*>(boardClassesBasic.data()), boardClassesBasic.size() * sizeof(uint16_t));
    std::unordered_map<uint64_t, uint8_t> handMetas;
    std::set<uint32_t> flopClasses;
    while (dealer.flopGen()) {
        if (!BASIC) {
            flopClasses.insert(boardClasses[dealer.getBoardIndex(1)]);
        } else {
            flopClasses.insert(boardClassesBasic[dealer.getBoardIndex(1)]);
        }
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

            std::unordered_map<uint64_t, std::array<float, 165>> flopHandDists;
            std::unordered_map<uint64_t, std::array<float, 165>> turnHandDists;
            std::unordered_map<uint64_t, std::array<float, 165>> riverHandDists;
            dealer = Dealer();
            while (dealer.flopGen()) {
                uint32_t flopClass = boardClasses[dealer.getBoardIndex(1)];
                uint32_t flopClassSig;
                if (!BASIC) {
                    flopClassSig = boardClasses[dealer.getBoardIndex(1)];
                } else {
                    flopClassSig = boardClassesBasic[dealer.getBoardIndex(1)];
                }
                if (flopClassSig != u) {
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
                    uint32_t turnClassSig;
                    if (!BASIC) {
                        turnClassSig = boardClasses[dealer.getBoardIndex(2)];
                    } else {
                        turnClassSig = boardClassesBasic[dealer.getBoardIndex(2)];
                    }
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
                        uint32_t riverClassSig;
                        if (!BASIC) {
                            riverClassSig = boardClasses[dealer.getBoardIndex(3)];
                        } else {
                            riverClassSig = boardClassesBasic[dealer.getBoardIndex(3)];
                        }
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
                            flopHandDists[uint64_t(flopClassSig) | uint64_t(flopClass) << 16
                                | uint64_t(flopHandClasses[i]) << 32][referenceRank]++;
                            turnHandDists[uint64_t(turnClassSig) | uint64_t(turnClass) << 16
                                | uint64_t(turnHandClasses[i]) << 32][referenceRank]++;
                            riverHandDists[uint64_t(riverClassSig) | uint64_t(riverClass) << 16
                                | uint64_t(riverHandClasses[i]) << 32][referenceRank]++;
                        }
                        dealer.riverSet();
                    }
                    dealer.turnSet();
                }
                dealer.flopSet();
            }
            std::unordered_map<uint64_t, uint8_t> localMetas;
            clusterDists(flopHandDists, localMetas, 32);
            clusterDists(turnHandDists, localMetas, 24);
            clusterDists(riverHandDists, localMetas, 16);
            std::lock_guard<std::mutex> lock(metaMutex);
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
    std::string name = !BASIC ? "solver/handAnalysis/handMetas.bin" : "solver/handAnalysis/handMetasBasic.bin";
    std::ofstream fileBasic(name, std::ios::binary);
    uint64_t size = handMetas.size();
    fileBasic.write(reinterpret_cast<const char*>(&size), sizeof(size));
    for (const auto& [key, value] : handMetas) {
        fileBasic.write(reinterpret_cast<const char*>(&key), sizeof(key));
        fileBasic.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    fileBasic.close();
    return 0;
}
