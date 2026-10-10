#include "solver/include/library.h"
#include "solver/handAnalysis/handClusterer.h"
#include "solver/handAnalysis/dealer.h"

// Compares two (board, hand) pairs and reports whether they share a board class and a
// hand cluster under the normal and the basic tables, i.e. whether the engine's
// setMetaState() would give them the same meta state.
//
// Build and run from the repo root (the tables are read from solver/handAnalysis/):
//   clang++ -std=c++20 -O2 -I. solver/handAnalysis/classCompare.cpp -o build/classCompare
//   build/classCompare Ks7h2d AcKc Qs8h3d AdKd
// With no arguments it prompts for lines of "board1 hand1 board2 hand2".
// Cards are rank AKQJT98765432 + suit cdhs; boards are 3, 4 or 5 cards.

const char CARD_RANKS[14] = "AKQJT98765432";
const char CARD_SUITS[5] = "cdhs";

struct Spot {
    std::array<int, 5> boardCards = {0, 0, 0, 0, 0};
    std::array<int, 2> holeCards;
    int street;
};

struct Lookup {
    uint16_t boardClass;
    uint16_t boardClassBasic;
    uint32_t handClass;
    int handMeta;
    int handMetaBasic;
};

std::vector<uint16_t> boardClasses;
std::vector<uint16_t> boardClassesBasic;
std::unordered_map<uint64_t, uint8_t> handMetas;
std::unordered_map<uint64_t, uint8_t> handMetasBasic;

void loadBoardClasses(const std::string& path, std::vector<uint16_t>& table) {
    table.resize(60929700);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(table.data()), table.size() * sizeof(uint16_t))) {
        throw std::runtime_error("could not read " + path);
    }
}

void loadHandMetas(const std::string& path, std::unordered_map<uint64_t, uint8_t>& out) {
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

// Sorts the flop and the hole cards the way the engine does: getBoardIndex() needs a
// sorted flop, and classifyHand() is sensitive to hole-card order.
Spot parseSpot(const std::string& boardText, const std::string& handText) {
    std::vector<int> board = parseCards(boardText);
    std::vector<int> hand = parseCards(handText);
    if (board.size() < 3 || board.size() > 5) {
        throw std::runtime_error("board '" + boardText + "' must have 3, 4 or 5 cards");
    }
    if (hand.size() != 2) {
        throw std::runtime_error("hand '" + handText + "' must have 2 cards");
    }
    std::set<int> seen;
    for (int card : board) seen.insert(card);
    for (int card : hand) seen.insert(card);
    if (seen.size() != board.size() + 2) {
        throw std::runtime_error("'" + boardText + " " + handText + "' repeats a card");
    }
    Spot spot;
    for (size_t i = 0; i < board.size(); i++) {
        spot.boardCards[i] = board[i];
    }
    std::sort(spot.boardCards.begin(), spot.boardCards.begin() + 3);
    spot.holeCards = {std::min(hand[0], hand[1]), std::max(hand[0], hand[1])};
    spot.street = int(board.size()) - 2;
    return spot;
}

int findMeta(const std::unordered_map<uint64_t, uint8_t>& metas, uint64_t key) {
    auto found = metas.find(key);
    return found == metas.end() ? -1 : found->second;
}

Lookup lookup(const Spot& spot) {
    Dealer dealer;
    HandClusterer handClusterer;
    dealer.boardCards = spot.boardCards;
    uint32_t boardIndex = dealer.getBoardIndex(spot.street);
    Lookup result;
    result.boardClass = boardClasses[boardIndex];
    result.boardClassBasic = boardClassesBasic[boardIndex];
    result.handClass = handClusterer.classifyHand(spot.boardCards, spot.holeCards, spot.street);
    // Both meta maps are keyed on the full board class, as in Engine::setMetaState().
    uint64_t key = uint64_t(result.boardClass) << 16 | uint64_t(result.handClass) << 32;
    result.handMeta = findMeta(handMetas, key);
    result.handMetaBasic = findMeta(handMetasBasic, key);
    return result;
}

std::string spotString(const Spot& spot) {
    static const char* streets[4] = {"", "flop", "turn", "river"};
    std::string text;
    for (int i = 0; i < spot.street + 2; i++) {
        text += cardString(spot.boardCards[i]);
    }
    return text + " " + cardString(spot.holeCards[0]) + cardString(spot.holeCards[1])
        + " (" + streets[spot.street] + ")";
}

std::string metaString(int meta) {
    return meta < 0 ? "missing" : std::to_string(meta);
}

void printRow(const std::string& label, const std::string& a, const std::string& b, bool same) {
    std::cout << "  " << std::left << std::setw(18) << label << std::right << std::setw(10) << a
        << std::setw(10) << b << "   " << (same ? "same" : "different") << "\n";
}

void compare(const std::string& board1, const std::string& hand1, const std::string& board2,
        const std::string& hand2) {
    Spot spot1 = parseSpot(board1, hand1);
    Spot spot2 = parseSpot(board2, hand2);
    Lookup a = lookup(spot1);
    Lookup b = lookup(spot2);

    bool sameBoard = a.boardClass == b.boardClass;
    bool sameBoardBasic = a.boardClassBasic == b.boardClassBasic;
    bool sameMeta = a.handMeta >= 0 && a.handMeta == b.handMeta;
    bool sameMetaBasic = a.handMetaBasic >= 0 && a.handMetaBasic == b.handMetaBasic;

    std::cout << "\n  1: " << spotString(spot1) << "\n  2: " << spotString(spot2) << "\n\n";
    std::cout << "  " << std::left << std::setw(18) << "" << std::right << std::setw(10) << "1"
        << std::setw(10) << "2" << "\n";
    printRow("hand class (raw)", std::to_string(a.handClass), std::to_string(b.handClass),
        a.handClass == b.handClass);
    std::cout << "  normal\n";
    printRow("  board class", std::to_string(a.boardClass), std::to_string(b.boardClass), sameBoard);
    printRow("  hand cluster", metaString(a.handMeta), metaString(b.handMeta), sameMeta);
    std::cout << "  basic\n";
    printRow("  board class", std::to_string(a.boardClassBasic), std::to_string(b.boardClassBasic),
        sameBoardBasic);
    printRow("  hand cluster", metaString(a.handMetaBasic), metaString(b.handMetaBasic), sameMetaBasic);
    // Cluster ids are numbered within a board class group, so they only mean the same
    // hand class when the board class matches too.
    std::cout << "\n  same board + hand class: normal " << (sameBoard && sameMeta ? "yes" : "no")
        << ", basic " << (sameBoardBasic && sameMetaBasic ? "yes" : "no") << "\n\n";
}

int main(int argc, char** argv) {
    if (argc != 1 && argc != 5) {
        std::cerr << "usage: " << argv[0] << " [board1 hand1 board2 hand2]\n";
        return 1;
    }
    try {
        loadBoardClasses("solver/handAnalysis/boardClasses.bin", boardClasses);
        loadBoardClasses("solver/handAnalysis/boardClassesBasic.bin", boardClassesBasic);
        loadHandMetas("solver/handAnalysis/handMetas.bin", handMetas);
        loadHandMetas("solver/handAnalysis/handMetasBasic.bin", handMetasBasic);
    } catch (const std::exception& e) {
        std::cerr << e.what() << " (run from the repo root)\n";
        return 1;
    }

    if (argc == 5) {
        try {
            compare(argv[1], argv[2], argv[3], argv[4]);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    std::string line;
    while (std::cout << "board1 hand1 board2 hand2> " << std::flush && std::getline(std::cin, line)) {
        std::istringstream words(line);
        std::string board1, hand1, board2, hand2, extra;
        if (!(words >> board1)) continue;
        if (board1 == "q" || board1 == "quit") break;
        if (!(words >> hand1 >> board2 >> hand2) || (words >> extra)) {
            std::cerr << "expected: board1 hand1 board2 hand2, e.g. Ks7h2d AcKc Qs8h3d AdKd\n";
            continue;
        }
        try {
            compare(board1, hand1, board2, hand2);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
        }
    }
    return 0;
}
