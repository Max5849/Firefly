#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"

// The metas stage to build (see stagePath): it reads boardClasses<letter>.bin and writes
// handDists<letter>.bin and handMetas<letter>.bin.
int STAGE = 2;
// True writes only preflopMetas.bin, the preflop clusters, and ignores STAGE and
// ACCUMULATE. That file has no stage letter: it reads no board class file, so no stage's
// tables change it. Its distance and strength weight each bucket by the square of its
// height, not linearly (bucketWeight).
bool PREFLOP = false;
// True enumerates every board and saves the histograms to the stage's handDists file;
// false reloads them and only clusters. Accumulate again whenever the board class file
// changes.
bool ACCUMULATE = false;

// The largest merge cost each street accepts, as a share of the board class's total
// runout weight. Uncalibrated: tune against the cluster counts clusterDists prints.
double FLOP_MAX_DISTANCE = 1.0;
double TURN_MAX_DISTANCE = 1.0;
double RIVER_MAX_DISTANCE = 1.0;
// The same, for the one division of 169 preflop classes.
double PREFLOP_MAX_DISTANCE = 30.0;
// solver/singleHand's hand cluster field is 6 bits, so no board class may keep more clusters.
const int MAX_CLUSTERS = 64;

// Showdown histograms keyed by boardClass | handClass << 16, where boardClass is the
// STAGE board class.
using HandDists = std::unordered_map<uint64_t, std::array<uint32_t, 165>>;

void computeCdf(const std::array<float, 165>& point, std::array<double, 165>& cdf) {
    double sum = 0;
    for (int p = 0; p < 165; p++) {
        sum += point[p];
        cdf[p] = sum;
    }
}

// A bucket's weight in cdfDistance and distStrength: its distance from the bottom,
// 164.5 - p, squared for preflop, so with the cdf's own sum the cost is cubic there
// rather than quadratic.
double bucketWeight(int p) {
    double weight = 164.5 - p;
    if (PREFLOP) {
        weight *= weight;
    }
    return weight;
}

double cdfDistance(const std::array<double, 165>& a, const std::array<double, 165>& b) {
    double distance = 0;
    for (int p = 0; p < 165; p++) {
        distance += bucketWeight(p) * std::abs(a[p] - b[p]);
    }
    return distance;
}

// The weighted cdf sum over its largest value (every hand in bucket 0), so 0..1.
float distStrength(const std::array<float, 165>& point) {
    double sum = 0;
    double strength = 0;
    double total = 0;
    for (int p = 0; p < 165; p++) {
        sum += point[p];
        strength += bucketWeight(p) * sum;
        total += bucketWeight(p);
    }
    return float(strength / total);
}

// Merges the cheapest pair until the next merge would cost more than maxDistance times the
// division's total weight, or, while more than MAX_CLUSTERS are left, whatever it costs.
// Returns the number of clusters left.
int mergeDivision(std::vector<std::vector<uint64_t>>& clusterHands, std::vector<uint64_t>& clusterSizes,
        std::vector<std::array<float, 165>>& clusterPoints, std::vector<char>& alive,
        int begin, int end, double maxDistance) {
    int n = end - begin;
    std::vector<std::array<double, 165>> cdfs(n);
    std::vector<int> nn(n);
    std::vector<double> nnDist(n);
    std::vector<double> strengths(n);
    double totalSize = 0;
    for (int i = 0; i < n; i++) {
        computeCdf(clusterPoints[begin + i], cdfs[i]);
        strengths[i] = distStrength(clusterPoints[begin + i]);
        totalSize += clusterSizes[begin + i];
    }
    // The linkage grows in proportion to the cluster sizes, and a flop class counts every
    // runout below it, so the threshold is scaled by the division's weight to fit every street.
    double maxLinkage = maxDistance * totalSize;
    // Ward's merge cost: the cdf distance scaled by na * nb / (na + nb), so merging two
    // large clusters costs more than folding a small one into a large one, and by
    // sqrt(sa^2 + sb^2) over distStrength, so any merge involving a strong cluster costs more.
    auto linkage = [&](int i, int j) {
        double na = clusterSizes[begin + i];
        double nb = clusterSizes[begin + j];
        double sa = strengths[i];
        double sb = strengths[j];
        return na * nb / (na + nb) * (sa * sa + sb * sb) * cdfDistance(cdfs[i], cdfs[j]);
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
    int live = n;
    for (; live > 1; live--) {
        int a = -1;
        double minDistance = std::numeric_limits<double>::max();
        for (int i = 0; i < n; i++) {
            if (alive[begin + i] && nnDist[i] < minDistance) {
                minDistance = nnDist[i];
                a = i;
            }
        }
        // Every other merge costs at least as much, so none would pass the threshold either.
        if (live <= MAX_CLUSTERS && minDistance > maxLinkage) {
            break;
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
    return live;
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

// Writes a uint64 record count, then each key followed by its 165 counts.
void writeDists(std::ofstream& file, const HandDists& handDists) {
    uint64_t size = handDists.size();
    file.write(reinterpret_cast<const char*>(&size), sizeof(size));
    for (const auto& [key, dist] : handDists) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key));
        file.write(reinterpret_cast<const char*>(dist.data()), sizeof(dist));
    }
}

// Reads the next block writeDists wrote. Returns false if the file ends early.
bool readDists(std::ifstream& file, HandDists& handDists) {
    uint64_t size = 0;
    file.read(reinterpret_cast<char*>(&size), sizeof(size));
    handDists.reserve(size);
    for (uint64_t i = 0; i < size && file; i++) {
        uint64_t key;
        std::array<uint32_t, 165> dist;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        file.read(reinterpret_cast<char*>(dist.data()), sizeof(dist));
        handDists.emplace(key, dist);
    }
    return bool(file);
}

// The 169 preflop classes for cards i < j: pairs and offsuit hands 0..90, suited hands
// 91..168. A copy of the formula in engine.h's loadTables(); keep the two identical, since
// preflopMetas.bin is indexed by it.
int preflopClass(int i, int j) {
    return (i / 4) * (25 - (i / 4)) / 2 + (j / 4) + (90 - (i / 4)) * (i % 4 == j % 4);
}

// The preflop clusters: every hole pair's showdown bucket on every distinct five-card
// board, tallied by preflop class, clustered as one division, and written as one cluster
// id per class to preflopMetas.bin. Reads no board class file. Returns false on failure.
bool clusterPreflop() {
    using PreflopDists = std::array<std::array<uint64_t, 165>, 169>;
    std::array<int, 1326> classOf;
    std::array<std::string, 169> names;
    const char* ranks = "AKQJT98765432";
    for (int i = 0; i < 51; i++) {
        for (int j = i + 1; j < 52; j++) {
            int c = preflopClass(i, j);
            classOf[i * (101 - i) / 2 + j - 1] = c;
            names[c] = std::string(1, ranks[i / 4]) + ranks[j / 4]
                + (i / 4 == j / 4 ? "" : i % 4 == j % 4 ? "s" : "o");
        }
    }

    // Flops are dealt sorted, so a turn above the flop and a river above the turn visit
    // each five-card board exactly once. Flops are dealt out round-robin across threads.
    unsigned numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<PreflopDists> threadDists(numThreads);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < numThreads; t++) {
        threads.emplace_back([&, t]() {
            Dealer dealer;
            PreflopDists& dists = threadDists[t];
            uint64_t flop = 0;
            while (dealer.flopGen()) {
                if (flop++ % numThreads != t) {
                    dealer.flopSet();
                    continue;
                }
                while (dealer.turnGen()) {
                    if (dealer.boardCards[3] > dealer.boardCards[2]) {
                        while (dealer.riverGen()) {
                            if (dealer.boardCards[4] > dealer.boardCards[3]) {
                                std::array<int, 1326> buckets = dealer.rankHands();
                                for (int i = 0; i < 1326; i++) {
                                    if (buckets[i] >= 0) {
                                        dists[classOf[i]][buckets[i]]++;
                                    }
                                }
                            }
                            dealer.riverSet();
                        }
                    }
                    dealer.turnSet();
                }
                dealer.flopSet();
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    std::vector<std::vector<uint64_t>> clusterHands(169);
    std::vector<uint64_t> clusterSizes(169);
    std::vector<std::array<float, 165>> clusterPoints(169);
    for (int c = 0; c < 169; c++) {
        std::array<uint64_t, 165> dist = {0};
        uint64_t sum = 0;
        for (const PreflopDists& dists : threadDists) {
            for (int p = 0; p < 165; p++) {
                dist[p] += dists[c][p];
            }
        }
        for (int p = 0; p < 165; p++) {
            sum += dist[p];
        }
        if (sum == 0) {
            std::cout << "preflop class " << names[c] << " has no showdowns\n";
            return false;
        }
        for (int p = 0; p < 165; p++) {
            clusterPoints[c][p] = float(double(dist[p]) / double(sum));
        }
        clusterHands[c] = {uint64_t(c)};
        clusterSizes[c] = sum;
    }
    std::vector<char> alive(169, 1);
    int live = mergeDivision(clusterHands, clusterSizes, clusterPoints, alive, 0, 169, PREFLOP_MAX_DISTANCE);

    std::array<uint8_t, 169> preflopClusters;
    std::cout << "preflop: " << live << " clusters\n";
    int cluster = 0;
    for (int i = 0; i < 169; i++) {
        if (!alive[i]) continue;
        std::cout << "  " << cluster << ":";
        for (uint64_t c : clusterHands[i]) {
            preflopClusters[c] = uint8_t(cluster);
            std::cout << " " << names[c];
        }
        std::cout << "\n";
        cluster++;
    }

    std::string name = "solver/handAnalysis/preflopMetas.bin";
    std::ofstream file(name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(preflopClusters.data()), preflopClusters.size());
    if (!file) {
        std::cout << "could not write " << name << "\n";
        return false;
    }
    return true;
}

// Clusters each board class's hands independently, spread across threads. A key that
// handMetas already holds is left alone and counted, since it means two streets share a
// board class id. Returns that count.
uint64_t clusterDists(HandDists& handDists, std::unordered_map<uint64_t, uint8_t>& handMetas, double maxDistance) {
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
    int minLive = MAX_CLUSTERS;
    int maxLive = 0;
    uint64_t totalLive = 0;
    int capped = 0;
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
            int live = mergeDivision(clusterHands, clusterSizes, clusterPoints, alive, 0, n, maxDistance);
            std::lock_guard<std::mutex> lock(metaMutex);
            minLive = std::min(minLive, live);
            maxLive = std::max(maxLive, live);
            totalLive += live;
            if (live == MAX_CLUSTERS) {
                capped++;
            }
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
    std::cout << "clusters per board class: min " << minLive << ", mean "
        << (divisions.empty() ? 0.0 : double(totalLive) / divisions.size()) << ", max " << maxLive
        << ", " << capped << " at the " << MAX_CLUSTERS << " cap\n";
    return collisions;
}

// Walks every board and fills each street's showdown histograms.
void accumulateDists(HandDists& flopHandDists, HandDists& turnHandDists, HandDists& riverHandDists) {
    Dealer dealer;
    std::vector<uint16_t> boardClasses;
    boardClasses.resize(60929700);
    // The stage 0 classes split the work below; the STAGE classes key the histograms.
    std::ifstream readFile(stagePath("boardClasses", 0), std::ios::binary);
    readFile.read(reinterpret_cast<char*>(boardClasses.data()), boardClasses.size() * sizeof(uint16_t));
    std::vector<uint16_t> boardClassesStage;
    if (STAGE != 0) {
        boardClassesStage.resize(60929700);
        std::ifstream otherFile(stagePath("boardClasses", STAGE), std::ios::binary);
        otherFile.read(reinterpret_cast<char*>(boardClassesStage.data()), boardClassesStage.size() * sizeof(uint16_t));
    }
    const std::vector<uint16_t>& classTable = STAGE == 0 ? boardClasses : boardClassesStage;

    // Work is split by full flop class only to divide the enumeration. Histograms are
    // merged into global maps and clustered once every board has been walked, so a board
    // class may draw its boards from any number of flops.
    std::set<uint32_t> flopClasses;
    while (dealer.flopGen()) {
        flopClasses.insert(boardClasses[dealer.getBoardIndex(1)]);
        dealer.flopSet();
    }

    std::vector<uint32_t> flopClassList(flopClasses.begin(), flopClasses.end());
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
}

int main() {
    if (PREFLOP) {
        return clusterPreflop() ? 0 : 1;
    }
    std::string distsName = stagePath("handDists", STAGE);
    HandDists flopHandDists;
    HandDists turnHandDists;
    HandDists riverHandDists;
    std::ifstream distsFile;
    if (ACCUMULATE) {
        accumulateDists(flopHandDists, turnHandDists, riverHandDists);
        std::ofstream writeFile(distsName, std::ios::binary);
        writeDists(writeFile, flopHandDists);
        writeDists(writeFile, turnHandDists);
        writeDists(writeFile, riverHandDists);
        if (!writeFile) {
            std::cout << "could not write " << distsName << "\n";
            return 1;
        }
    } else {
        distsFile.open(distsName, std::ios::binary);
        if (!distsFile) {
            std::cout << "could not open " << distsName << "; run once with ACCUMULATE = true\n";
            return 1;
        }
    }
    // When reloading, each street is read just before it is clustered, so only one is held at a time.
    auto load = [&](HandDists& handDists) {
        if (!ACCUMULATE && !readDists(distsFile, handDists)) {
            std::cout << distsName << " ended early\n";
            return false;
        }
        return true;
    };

    std::unordered_map<uint64_t, uint8_t> handMetas;
    uint64_t collisions = 0;
    if (!load(flopHandDists)) return 1;
    std::cout << "clustering flop: ";
    collisions += clusterDists(flopHandDists, handMetas, FLOP_MAX_DISTANCE);
    flopHandDists = HandDists();
    if (!load(turnHandDists)) return 1;
    std::cout << "clustering turn: ";
    collisions += clusterDists(turnHandDists, handMetas, TURN_MAX_DISTANCE);
    turnHandDists = HandDists();
    if (!load(riverHandDists)) return 1;
    std::cout << "clustering river: ";
    collisions += clusterDists(riverHandDists, handMetas, RIVER_MAX_DISTANCE);
    riverHandDists = HandDists();
    if (collisions > 0) {
        std::cout << "warning: " << collisions << " keys share a board class id across streets;"
            << " the first street clustered kept its cluster id\n";
    }

    std::string name = stagePath("handMetas", STAGE);
    std::ofstream metasFile(name, std::ios::binary);
    uint64_t size = handMetas.size();
    metasFile.write(reinterpret_cast<const char*>(&size), sizeof(size));
    for (const auto& [key, value] : handMetas) {
        metasFile.write(reinterpret_cast<const char*>(&key), sizeof(key));
        metasFile.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    metasFile.close();
    return 0;
}
