#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"

bool BASIC = true;

// Showdown histograms keyed by boardClass | handClass << 16, where boardClass is the full
// class, or the basic class when BASIC is set.
using HandDists = std::unordered_map<uint64_t, std::array<uint32_t, 165>>;

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
        return na * nb / (na + nb) * std::sqrt(sa * sa + sb * sb) * cdfDistance(cdfs[i], cdfs[j]);
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

// Moves every histogram in from into into, summing those whose key both hold.
void mergeDists(HandDists& into, HandDists& from) {
    into.merge(from);
    for (auto& [key, dist] : from) {
        std::array<uint32_t, 165>& target = into[key];
        for (int p = 0; p < 165; p++) {
            target[p] += dist[p];
        }
    }
    from.clear();
}

// Clusters each board class's hands independently, spread across threads. A key that
// handMetas already holds is left alone and counted, since it means two streets share a
// board class id. Returns that count.
uint64_t clusterDists(HandDists& handDists, std::unordered_map<uint64_t, uint8_t>& handMetas, int maxClusters) {
    std::vector<std::pair<uint64_t, const std::array<uint32_t, 165>*>> entries;
    entries.reserve(handDists.size());
    for (auto& q : handDists) {
        entries.push_back({q.first, &q.second});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& x, const auto& y) {
        uint16_t bx = static_cast<uint16_t>(x.first), by = static_cast<uint16_t>(y.first);
        return bx != by ? bx < by : x.first < y.first;
    });
    std::vector<std::pair<size_t, size_t>> divisions;
    for (size_t i = 0; i < entries.size();) {
        size_t begin = i;
        uint16_t boardClass = static_cast<uint16_t>(entries[i].first);
        while (i < entries.size() && static_cast<uint16_t>(entries[i].first) == boardClass) {
            i++;
        }
        divisions.push_back({begin, i});
    }
    // Largest first, so a big division does not start last and run alone.
    std::sort(divisions.begin(), divisions.end(), [](const auto& x, const auto& y) {
        return x.second - x.first > y.second - y.first;
    });
    std::cout << entries.size() << " keys in " << divisions.size() << " board classes, largest "
        << (divisions.empty() ? 0 : divisions[0].second - divisions[0].first) << "\n";

    std::mutex metaMutex;
    std::atomic<size_t> nextDivision{0};
    uint64_t collisions = 0;
    auto worker = [&]() {
        while (true) {
            size_t d = nextDivision.fetch_add(1);
            if (d >= divisions.size()) break;
            auto [begin, end] = divisions[d];
            int n = static_cast<int>(end - begin);
            std::vector<std::vector<uint64_t>> clusterHands(n);
            std::vector<uint64_t> clusterSizes(n);
            std::vector<std::array<float, 165>> clusterPoints(n);
            for (int i = 0; i < n; i++) {
                const auto& [key, dist] = entries[begin + i];
                clusterHands[i] = {key};
                uint64_t sum = 0;
                for (int p = 0; p < 165; p++) {
                    sum += (*dist)[p];
                }
                for (int p = 0; p < 165; p++) {
                    clusterPoints[i][p] = float(double((*dist)[p]) / double(sum));
                }
                clusterSizes[i] = sum;
            }
            std::vector<char> alive(n, 1);
            mergeDivision(clusterHands, clusterSizes, clusterPoints, alive, 0, n, maxClusters);
            std::lock_guard<std::mutex> lock(metaMutex);
            int cluster = 0;
            for (int i = 0; i < n; i++) {
                if (!alive[i]) continue;
                for (uint64_t key : clusterHands[i]) {
                    if (!handMetas.try_emplace(key, cluster).second) {
                        collisions++;
                    }
                }
                cluster++;
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); t++) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
    return collisions;
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
    const std::vector<uint16_t>& classTable = !BASIC ? boardClasses : boardClassesBasic;

    // Work is split by full flop class only to divide the enumeration. Histograms are
    // merged into global maps and clustered once every board has been walked, so a board
    // class may draw its boards from any number of flops.
    std::set<uint32_t> flopClasses;
    while (dealer.flopGen()) {
        flopClasses.insert(boardClasses[dealer.getBoardIndex(1)]);
        dealer.flopSet();
    }

    std::vector<uint32_t> flopClassList(flopClasses.begin(), flopClasses.end());
    HandDists flopHandDists;
    HandDists turnHandDists;
    HandDists riverHandDists;
    std::mutex distMutex;
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

            HandDists localFlopDists;
            HandDists localTurnDists;
            HandDists localRiverDists;
            dealer = Dealer();
            while (dealer.flopGen()) {
                if (boardClasses[dealer.getBoardIndex(1)] != u) {
                    dealer.flopSet();
                    continue;
                }
                uint64_t flopClass = classTable[dealer.getBoardIndex(1)];
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
                    uint64_t turnClass = classTable[dealer.getBoardIndex(2)];
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
                        uint64_t riverClass = classTable[dealer.getBoardIndex(3)];
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
                            localFlopDists[flopClass | uint64_t(flopHandClasses[i]) << 16][referenceRank]++;
                            localTurnDists[turnClass | uint64_t(turnHandClasses[i]) << 16][referenceRank]++;
                            localRiverDists[riverClass | uint64_t(riverHandClasses[i]) << 16][referenceRank]++;
                        }
                        dealer.riverSet();
                    }
                    dealer.turnSet();
                }
                dealer.flopSet();
            }
            std::lock_guard<std::mutex> lock(distMutex);
            mergeDists(flopHandDists, localFlopDists);
            mergeDists(turnHandDists, localTurnDists);
            mergeDists(riverHandDists, localRiverDists);
            std::cout << ++done << " / " << flopClassList.size() << " flop classes accumulated\n";
        }
    };

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < numThreads; t++) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    std::unordered_map<uint64_t, uint8_t> handMetas;
    uint64_t collisions = 0;
    std::cout << "clustering flop: ";
    collisions += clusterDists(flopHandDists, handMetas, 24);
    flopHandDists = HandDists();
    std::cout << "clustering turn: ";
    collisions += clusterDists(turnHandDists, handMetas, 18);
    turnHandDists = HandDists();
    std::cout << "clustering river: ";
    collisions += clusterDists(riverHandDists, handMetas, 12);
    riverHandDists = HandDists();
    if (collisions > 0) {
        std::cout << "warning: " << collisions << " keys share a board class id across streets;"
            << " the first street clustered kept its cluster id\n";
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
