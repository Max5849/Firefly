#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"
#include <map>

// Prints example hands for every hand cluster of a board class: enter a flop, turn or
// river, and it finds the boards that share its board class (normal or basic tables) and
// lists up to EXAMPLES (board, hole cards) pairs per hand cluster, spread across boards.
// Each cluster also gets its showdown distribution, rebuilt from sampled runouts, drawn
// as a histogram from the nuts (bucket 0) on the left to the weakest bucket on the right.
//
// Build and run from the repo root (the tables are read from solver/handAnalysis/):
//   clang++ -std=c++20 -O2 -I. solver/handAnalysis/classCompare.cpp -o build/classCompare
//   build/classCompare Ks7h2d            normal tables
//   build/classCompare Ks7h2d4c basic    basic tables
// With no arguments it prompts for lines of "board [basic]".
// Cards are rank AKQJT98765432 + suit cdhs; boards are 3, 4 or 5 cards.

const int EXAMPLES = 40;
// Boards sampled per query; river classes can span far more boards than are worth classifying.
const size_t MAX_BOARDS = 20000;
// Showdown rankings per query used to rebuild the cluster distributions, and the most
// boards they are spread across; flops and turns get random runouts to reach the total.
const size_t DIST_RANKINGS = 20000;
const size_t DIST_BOARDS = 2000;

const char CARD_RANKS[14] = "AKQJT98765432";
const char CARD_SUITS[5] = "cdhs";

struct Board {
    std::array<int, 5> cards;
    uint16_t boardClass;
};

std::vector<uint16_t> boardClasses;
std::vector<uint16_t> boardClassesBasic;
ankerl::unordered_dense::map<uint64_t, uint8_t> handMetas;
ankerl::unordered_dense::map<uint64_t, uint8_t> handMetasBasic;
std::vector<uint32_t> handScores;

void loadHandScores(const std::string& path) {
    handScores.resize(133784560);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(handScores.data()), handScores.size() * sizeof(uint32_t))) {
        throw std::runtime_error("could not read " + path);
    }
}

// Dealer::rankHands() for any river board, scored from handScores.bin: each hole pair's
// 0..164 showdown bucket against the opponent pairs it does not block, -1 for pairs that
// touch the board.
std::array<int, 1326> rankBuckets(const std::array<int, 5>& board) {
    static Dealer scorer;
    scorer.boardCards = board;
    bool onBoard[52] = {false};
    for (int card : board) {
        onBoard[card] = true;
    }
    std::array<std::array<int, 4>, 1081> hands;
    int count = 0;
    for (int i = 0; i < 51; i++) {
        if (onBoard[i]) continue;
        for (int j = i + 1; j < 52; j++) {
            if (onBoard[j]) continue;
            scorer.holeCards = {i, j};
            hands[count++] = {i * (101 - i) / 2 + j - 1, i, j, int(handScores[scorer.getHandIndex()])};
        }
    }
    std::sort(hands.begin(), hands.end(), [](const std::array<int, 4>& a, const std::array<int, 4>& b) {
        return a[3] > b[3];
    });
    std::array<int, 1326> buckets;
    buckets.fill(-1);
    std::array<int, 52> cardOverlaps = {0};
    int handRankIndex = 0;
    for (int i = 0; i < 1081; i++) {
        if (i > 0 && hands[i][3] != hands[i - 1][3]) {
            for (int j = handRankIndex; j < i; j++) {
                cardOverlaps[hands[j][1]]++;
                cardOverlaps[hands[j][2]]++;
            }
            handRankIndex = i;
        }
        int handRank = handRankIndex - cardOverlaps[hands[i][1]] - cardOverlaps[hands[i][2]];
        buckets[hands[i][0]] = std::min(handRank / 6, 164);
    }
    return buckets;
}

// The equityClusterer distStrength: 1 for a distribution entirely in bucket 0, near 0 for
// one entirely in bucket 164.
double distStrength(const std::array<double, 165>& dist) {
    double total = 0;
    for (double count : dist) {
        total += count;
    }
    double sum = 0;
    double strength = 0;
    for (int p = 0; p < 165; p++) {
        sum += dist[p] / total;
        strength += (164.5 - p) * sum;
    }
    return strength / (165 * 164.5 - 164 * 165 / 2);
}

const int DIST_COLUMNS = 83;
const int DIST_ROWS = 22;

// The distribution as probability per column, summing to 1. The 165 buckets are spread
// evenly over DIST_COLUMNS columns; a bucket that straddles a column boundary is split
// between the two columns in proportion to its overlap with each.
std::array<double, DIST_COLUMNS> distColumns(const std::array<double, 165>& dist) {
    double total = 0;
    for (double count : dist) {
        total += count;
    }
    std::array<double, DIST_COLUMNS> columns = {0};
    double width = 165.0 / DIST_COLUMNS;
    for (int c = 0; c < DIST_COLUMNS; c++) {
        double from = c * width;
        double to = from + width;
        for (int p = int(from); p < 165 && p < to; p++) {
            double overlap = std::min<double>(p + 1, to) - std::max<double>(p, from);
            columns[c] += dist[p] / total * overlap;
        }
    }
    return columns;
}

// DIST_ROWS rows of block characters with the nuts on the left, on a fixed scale from 0%
// of the cluster's hands at the bottom to 100% at the top, so every histogram encloses
// the same area and histograms compare across queries.
void printDistribution(const std::array<double, DIST_COLUMNS>& columns) {
    static const char* blocks[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    for (int r = DIST_ROWS - 1; r >= 0; r--) {
        std::cout << "    │";
        for (int c = 0; c < DIST_COLUMNS; c++) {
            int level = int(std::lround(columns[c] * DIST_ROWS * 8));
            std::cout << blocks[std::clamp(level - r * 8, 0, 8)];
        }
        std::cout << "│\n";
    }
    std::cout << "     nuts" << std::string(DIST_COLUMNS - 8, ' ') << "weak\n";
}

void loadBoardClasses(const std::string& path, std::vector<uint16_t>& table) {
    table.resize(60929700);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(table.data()), table.size() * sizeof(uint16_t))) {
        throw std::runtime_error("could not read " + path);
    }
}

void loadHandMetas(const std::string& path, ankerl::unordered_dense::map<uint64_t, uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    uint64_t size;
    if (!file.read(reinterpret_cast<char*>(&size), sizeof(size))) {
        throw std::runtime_error("could not read " + path);
    }
    out.reserve(size);
    for (uint64_t i = 0; i < size; i++) {
        uint64_t key;
        uint8_t value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        out[key] = value;
    }
    if (!file) {
        throw std::runtime_error(path + " is truncated");
    }
}

std::vector<int> parseCards(const std::string& text) {
    if (text.size() % 2 != 0) {
        throw std::runtime_error("'" + text + "' is not a list of two-character cards");
    }
    std::vector<int> cards;
    for (size_t i = 0; i < text.size(); i += 2) {
        const char* rankAt = std::find(CARD_RANKS, CARD_RANKS + 13, std::toupper(text[i]));
        const char* suitAt = std::find(CARD_SUITS, CARD_SUITS + 4, std::tolower(text[i + 1]));
        if (rankAt == CARD_RANKS + 13 || suitAt == CARD_SUITS + 4) {
            throw std::runtime_error("bad card '" + text.substr(i, 2) + "' (ranks AKQJT98765432, suits cdhs)");
        }
        cards.push_back(int(rankAt - CARD_RANKS) * 4 + int(suitAt - CARD_SUITS));
    }
    return cards;
}

std::string cardString(int card) {
    return std::string{CARD_RANKS[card / 4], CARD_SUITS[card % 4]};
}

std::string boardString(const std::array<int, 5>& cards, int street) {
    std::string text;
    for (int i = 0; i < street + 2; i++) {
        text += cardString(cards[i]);
    }
    return text;
}

// Every board of the street whose class in the chosen tables matches the input's. Board
// classes are cumulative, so only flops and turns that share the input's flop and turn
// classes need to be walked.
std::vector<Board> matchingBoards(const std::array<int, 5>& input, int street, bool basic) {
    const std::vector<uint16_t>& table = basic ? boardClassesBasic : boardClasses;
    Dealer dealer;
    dealer.boardCards = input;
    std::array<uint16_t, 4> targets;
    for (int s = 1; s <= street; s++) {
        targets[s] = table[dealer.getBoardIndex(s)];
    }
    std::vector<Board> boards;
    auto add = [&]() {
        boards.push_back({dealer.boardCards, boardClasses[dealer.getBoardIndex(street)]});
    };
    dealer = Dealer();
    while (dealer.flopGen()) {
        if (table[dealer.getBoardIndex(1)] == targets[1]) {
            if (street == 1) {
                add();
            }
            while (street > 1 && dealer.turnGen()) {
                if (table[dealer.getBoardIndex(2)] == targets[2]) {
                    if (street == 2) {
                        add();
                    }
                    while (street > 2 && dealer.riverGen()) {
                        if (table[dealer.getBoardIndex(3)] == targets[3]) {
                            add();
                        }
                        dealer.riverSet();
                    }
                }
                dealer.turnSet();
            }
        }
        dealer.flopSet();
    }
    return boards;
}

void showClusters(const std::string& boardText, bool basic) {
    std::vector<int> cards = parseCards(boardText);
    if (cards.size() < 3 || cards.size() > 5) {
        throw std::runtime_error("board '" + boardText + "' must have 3, 4 or 5 cards");
    }
    if (std::set<int>(cards.begin(), cards.end()).size() != cards.size()) {
        throw std::runtime_error("board '" + boardText + "' repeats a card");
    }
    std::array<int, 5> input = {0, 0, 0, 0, 0};
    std::copy(cards.begin(), cards.end(), input.begin());
    // getBoardIndex() needs a sorted flop, as the engine deals it.
    std::sort(input.begin(), input.begin() + 3);
    int street = int(cards.size()) - 2;

    const auto& metas = basic ? handMetasBasic : handMetas;
    if (metas.empty()) {
        throw std::runtime_error(std::string(basic ? "handMetasBasic.bin" : "handMetas.bin") + " is not loaded");
    }
    std::vector<Board> boards = matchingBoards(input, street, basic);
    // Every cluster any of these boards can produce, so the scan can stop once all are full.
    std::set<uint16_t> fullClasses;
    for (const Board& board : boards) {
        fullClasses.insert(board.boardClass);
    }
    std::map<int, std::vector<std::string>> examples;
    for (const auto& [key, meta] : metas) {
        if (fullClasses.count(uint16_t(key >> 16))) {
            examples[meta];
        }
    }

    std::mt19937 rng(std::random_device{}());
    std::shuffle(boards.begin(), boards.end(), rng);
    size_t sampled = std::min(boards.size(), MAX_BOARDS);
    std::vector<std::array<int, 2>> holes;
    for (int i = 0; i < 51; i++) {
        for (int j = i + 1; j < 52; j++) {
            holes.push_back({i, j});
        }
    }
    HandClusterer handClusterer;
    std::set<uint64_t> used;
    auto full = [&]() {
        for (auto& [meta, list] : examples) {
            if (list.size() < EXAMPLES) return false;
        }
        return true;
    };
    // The first pass takes at most one example per board per cluster to spread them
    // across boards; the second fills clusters that are still short.
    for (int pass = 0; pass < 2 && !full(); pass++) {
        for (size_t b = 0; b < sampled && !full(); b++) {
            const Board& board = boards[b];
            std::shuffle(holes.begin(), holes.end(), rng);
            std::set<int> taken;
            for (const std::array<int, 2>& hole : holes) {
                bool clash = false;
                for (int k = 0; k < street + 2; k++) {
                    clash |= board.cards[k] == hole[0] || board.cards[k] == hole[1];
                }
                uint64_t id = b * 1326 + hole[0] * (101 - hole[0]) / 2 + hole[1] - 1;
                if (clash || used.count(id)) continue;
                uint32_t handClass = handClusterer.classifyHand(board.cards, hole, street);
                auto found = metas.find(uint64_t(board.boardClass) << 16 | uint64_t(handClass) << 32);
                int meta = found == metas.end() ? -1 : found->second;
                std::vector<std::string>& list = examples[meta];
                if (list.size() >= EXAMPLES || (pass == 0 && taken.count(meta))) continue;
                taken.insert(meta);
                used.insert(id);
                list.push_back(boardString(board.cards, street) + " " + cardString(hole[0]) + cardString(hole[1]));
            }
        }
    }

    // Each cluster's showdown distribution, built the way equityClusterer builds it: every
    // pair on a board counts once per runout, at its bucket against the pairs it doesn't block.
    std::map<int, std::array<double, 165>> dists;
    size_t distBoards = std::min(boards.size(), DIST_BOARDS);
    size_t runouts = street == 3 ? 1 : std::max<size_t>(1, DIST_RANKINGS / std::max<size_t>(distBoards, 1));
    for (size_t b = 0; b < distBoards; b++) {
        const Board& board = boards[b];
        std::array<int, 1326> clusterOf;
        clusterOf.fill(INT_MIN);
        bool onBoard[52] = {false};
        for (int k = 0; k < street + 2; k++) {
            onBoard[board.cards[k]] = true;
        }
        std::vector<int> deck;
        for (int card = 0; card < 52; card++) {
            if (!onBoard[card]) {
                deck.push_back(card);
            }
        }
        for (const std::array<int, 2>& hole : holes) {
            if (onBoard[hole[0]] || onBoard[hole[1]]) continue;
            uint32_t handClass = handClusterer.classifyHand(board.cards, hole, street);
            auto found = metas.find(uint64_t(board.boardClass) << 16 | uint64_t(handClass) << 32);
            clusterOf[hole[0] * (101 - hole[0]) / 2 + hole[1] - 1] = found == metas.end() ? -1 : found->second;
        }
        for (size_t r = 0; r < runouts; r++) {
            std::array<int, 5> full = board.cards;
            for (int k = street + 2; k < 5; k++) {
                std::uniform_int_distribution<size_t> pick(k - street - 2, deck.size() - 1);
                std::swap(deck[k - street - 2], deck[pick(rng)]);
                full[k] = deck[k - street - 2];
            }
            std::array<int, 1326> buckets = rankBuckets(full);
            for (int i = 0; i < 1326; i++) {
                if (buckets[i] >= 0 && clusterOf[i] != INT_MIN) {
                    dists[clusterOf[i]][buckets[i]]++;
                }
            }
        }
    }

    static const char* streets[4] = {"", "flop", "turn", "river"};
    Dealer dealer;
    dealer.boardCards = input;
    uint32_t boardIndex = dealer.getBoardIndex(street);
    std::cout << "\n" << boardString(input, street) << " (" << streets[street] << "), "
        << (basic ? "basic board class " + std::to_string(boardClassesBasic[boardIndex])
            : "normal board class " + std::to_string(boardClasses[boardIndex]))
        << ": " << boards.size() << " boards";
    if (sampled < boards.size()) {
        std::cout << " (" << sampled << " sampled)";
    }
    std::cout << ", " << examples.size() << " hand clusters";
    if (distBoards > 0) {
        std::cout << "; distributions from " << distBoards * runouts << " showdowns over "
            << distBoards << " boards";
    }
    std::cout << "\n";
    if (!dists.empty()) {
        std::cout << "histograms: each column is the share of a cluster's hands in " << std::setprecision(3)
            << 165.0 / DIST_COLUMNS << std::defaultfloat << " buckets, from 0% at the "
            << "bottom to 100% at the top\n";
    }
    for (auto& [meta, list] : examples) {
        std::cout << "\n  cluster " << (meta < 0 ? "missing" : std::to_string(meta))
            << " (" << list.size() << ")";
        auto dist = dists.find(meta);
        if (dist == dists.end()) {
            std::cout << ", no showdowns sampled\n";
        } else {
            std::cout << ", strength " << std::fixed << std::setprecision(3) << distStrength(dist->second)
                << std::defaultfloat << "\n";
            printDistribution(distColumns(dist->second));
        }
        for (size_t i = 0; i < list.size(); i++) {
            std::cout << (i % 4 == 0 ? "    " : "   ") << std::left << std::setw(16) << list[i]
                << std::right << (i % 4 == 3 || i + 1 == list.size() ? "\n" : "");
        }
    }
    std::cout << "\n";
}

bool parseMode(const std::string& word) {
    if (word == "basic" || word == "b") return true;
    if (word == "normal" || word == "n") return false;
    throw std::runtime_error("mode '" + word + "' must be basic or normal");
}

int main(int argc, char** argv) {
    if (argc > 3) {
        std::cerr << "usage: " << argv[0] << " [board [basic|normal]]\n";
        return 1;
    }
    try {
        loadBoardClasses("solver/handAnalysis/boardClasses.bin", boardClasses);
        loadBoardClasses("solver/handAnalysis/boardClassesBasic.bin", boardClassesBasic);
        loadHandMetas("solver/handAnalysis/handMetas.bin", handMetas);
        loadHandScores("solver/handAnalysis/handScores.bin");
    } catch (const std::exception& e) {
        std::cerr << e.what() << " (run from the repo root)\n";
        return 1;
    }
    // The basic tables are only needed for basic queries, so a missing file is not fatal.
    try {
        loadHandMetas("solver/handAnalysis/handMetasBasic.bin", handMetasBasic);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "; basic queries are unavailable\n";
    }

    if (argc > 1) {
        try {
            showClusters(argv[1], argc == 3 && parseMode(argv[2]));
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    std::string line;
    while (std::cout << "board [basic]> " << std::flush && std::getline(std::cin, line)) {
        std::istringstream words(line);
        std::string board, mode, extra;
        if (!(words >> board)) continue;
        if (board == "q" || board == "quit") break;
        words >> mode;
        if (words >> extra) {
            std::cerr << "expected: board [basic], e.g. Ks7h2d or Ks7h2d4c basic\n";
            continue;
        }
        try {
            showClusters(board, !mode.empty() && parseMode(mode));
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
        }
    }
    return 0;
}
