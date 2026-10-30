// Firefly strategy API.
//
//   build/server [--host H] [--port P] [--threads N] [--warm]
//
// Must be run from the repository root: the engine resolves the handAnalysis tables,
// and the registry the strategy files, through relative paths.
//
// POST /strategy  {"config": {"players": 3, "chips": 200, "metasBasic": true,
//                             "memoryBasic": true},
//                  "actions": [2, 1], "board": "As7h2d"}
//   -> the table (game), the acting seat's key decoded (key), and the strategy, range,
//      EV and variances of all 1326 hole card combinations at that node (spot).
// GET  /health
// GET  /meta

#define CPPHTTPLIB_ZLIB_SUPPORT

#include "server/replay.h"
#include "server/keyLayout.h"
#include "server/spot.h"
#include "server/strategyStore.h"
#include "server/include/httplib.h"
#include "server/include/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

using json = nlohmann::json;

// The response is 1326 rows of a dozen numbers. A generic serialiser is the dominant
// cost of an otherwise few millisecond request, so it is written straight into one
// preallocated buffer with to_chars.
struct JsonWriter {
    std::string out;
    // Whether the next value in the current object or array needs a comma first.
    std::vector<bool> needComma;

    JsonWriter() { out.reserve(1 << 20); }

    void separate() {
        if (!needComma.empty()) {
            if (needComma.back()) {
                out += ',';
            }
            needComma.back() = true;
        }
    }

    void open(char bracket) {
        out += bracket;
        needComma.push_back(false);
    }

    void close(char bracket) {
        out += bracket;
        needComma.pop_back();
    }

    void beginObject() { separate(); open('{'); }
    void endObject() { close('}'); }
    void beginArray() { separate(); open('['); }
    void endArray() { close(']'); }

    // A key inside an object; the value that follows must not separate again.
    void key(const char* name) {
        separate();
        out += '"';
        out += name;
        out += "\":";
        needComma.back() = false;
    }

    void finishValue() {
        if (!needComma.empty()) {
            needComma.back() = true;
        }
    }

    void null() { separate(); out += "null"; finishValue(); }
    void boolean(bool value) { separate(); out += value ? "true" : "false"; finishValue(); }

    void number(double value) {
        separate();
        if (!std::isfinite(value)) {
            // Neither inf nor nan is valid JSON.
            out += "null";
        } else {
            char buffer[40];
            auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            out.append(buffer, end);
        }
        finishValue();
    }

    void integer(int64_t value) {
        separate();
        char buffer[24];
        auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out.append(buffer, end);
        finishValue();
    }

    // Keys are hex strings: a uint64 exceeds JSON's safe integer range and a
    // JavaScript client would silently round it.
    void hex(uint64_t value) {
        separate();
        char buffer[20] = "\"0x";
        auto [end, ec] = std::to_chars(buffer + 3, buffer + 19, value, 16);
        *end = '"';
        out.append(buffer, end + 1);
        finishValue();
    }

    void string(const std::string& text) {
        separate();
        out += '"';
        for (char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        out += '"';
        finishValue();
    }

    template <typename T>
    void field(const char* name, const T& value) {
        key(name);
        if constexpr (std::is_same_v<T, bool>) {
            boolean(value);
        } else if constexpr (std::is_integral_v<T>) {
            integer(int64_t(value));
        } else if constexpr (std::is_floating_point_v<T>) {
            number(double(value));
        } else {
            string(value);
        }
    }
};

struct ServerOptions {
    std::string host = "127.0.0.1";
    int port = 8080;
    int threads = 0;
    bool warm = false;
};

// Which sections the caller asked for. With no "include", everything.
struct IncludeMask {
    bool game = true;
    bool key = true;
    bool spot = true;
};

// The inverse temperatures per pot a request may ask for: ln(2^5) to ln(2^80). The
// singleHand trainer runs at ln(2^20).
inline const double MIN_INV_TEMP = 5 * std::log(2.0);
inline const double MAX_INV_TEMP = 80 * std::log(2.0);

struct Request {
    StrategyConfig config;
    double invTemp = DEFAULT_INV_TEMP;
    std::vector<int> actions;
    std::vector<int> board;
    IncludeMask include;
};

inline constexpr int MAX_CHIPS = 100000;

static json configJson(const StrategyConfig& config) {
    return {{"players", config.players}, {"chips", config.chips},
        {"metasBasic", config.metasBasic}, {"memoryBasic", config.memoryBasic},
        {"spec", config.spec()}};
}

static json availableJson() {
    json list = json::array();
    for (const StrategyConfig& config : availableStrategies()) {
        list.push_back(configJson(config));
    }
    return list;
}

static void fail(httplib::Response& response, int status, const std::string& code,
        const std::string& message, json extra = json::object()) {
    json body = std::move(extra);
    body["code"] = code;
    body["error"] = message;
    response.status = status;
    response.set_content(body.dump(), "application/json");
}

// Every field optional; a missing one takes the value config.h was compiled with.
static bool parseConfig(const json& body, StrategyConfig& config, std::string& error) {
    if (!body.contains("config") || body["config"].is_null()) {
        return true;
    }
    const json& node = body["config"];
    if (!node.is_object()) {
        error = "\"config\" must be an object like {\"players\": 3, \"chips\": 200, "
            "\"metasBasic\": true, \"memoryBasic\": true}";
        return false;
    }
    for (auto entry = node.begin(); entry != node.end(); ++entry) {
        const std::string& name = entry.key();
        const json& value = entry.value();
        if (name == "players") {
            if (!value.is_number_integer() || value.get<int>() < 3 || value.get<int>() > 7) {
                error = "\"config.players\" must be an integer from 3 to 7";
                return false;
            }
            config.players = value.get<int>();
        } else if (name == "chips") {
            if (!value.is_number() || value.get<double>() != std::floor(value.get<double>())
                    || value.get<double>() <= 0 || value.get<double>() > MAX_CHIPS) {
                error = "\"config.chips\" must be a positive whole number of chips up to "
                    + std::to_string(MAX_CHIPS);
                return false;
            }
            const int chips = int(value.get<double>());
            if (chips % 2 != 0) {
                error = "\"config.chips\" must be even: strategy files are named by "
                    "10 * chips + 10 * metasBasic, so an odd stack shares its name with "
                    "the next stack up";
                return false;
            }
            config.chips = chips;
        } else if (name == "metasBasic" || name == "memoryBasic") {
            if (!value.is_boolean()) {
                error = "\"config." + name + "\" must be true or false";
                return false;
            }
            (name == "metasBasic" ? config.metasBasic : config.memoryBasic) = value.get<bool>();
        } else {
            error = "unknown field \"" + name + "\" in \"config\" "
                "(valid: players, chips, metasBasic, memoryBasic)";
            return false;
        }
    }
    return true;
}

// An unknown section is a 400 rather than a silent omission: a typo would otherwise
// return a response missing a section and read as a working request.
static bool parseInclude(const json& body, IncludeMask& include, std::string& error) {
    if (!body.contains("include") || body["include"].is_null()) {
        return true;
    }
    const json& node = body["include"];
    if (!node.is_object()) {
        error = "\"include\" must be an object, for example {\"game\": true, \"spot\": false}";
        return false;
    }
    include = {false, false, false};
    for (auto entry = node.begin(); entry != node.end(); ++entry) {
        const std::string& name = entry.key();
        bool* target = name == "game" ? &include.game
                     : name == "key"  ? &include.key
                     : name == "spot" ? &include.spot
                     : nullptr;
        if (target == nullptr) {
            error = "unknown section \"" + name + "\" in \"include\" (valid: game, key, spot)";
            return false;
        }
        if (!entry.value().is_boolean()) {
            error = "\"include." + name + "\" must be true or false";
            return false;
        }
        *target = entry.value().get<bool>();
    }
    return true;
}

static void writeConfig(JsonWriter& w, const StrategyConfig& config) {
    w.beginObject();
    w.field("players", config.players);
    w.field("chips", config.chips);
    w.field("metasBasic", config.metasBasic);
    w.field("memoryBasic", config.memoryBasic);
    w.field("spec", config.spec());
    w.endObject();
}

static void writeLegal(JsonWriter& w, const std::array<bool, 5>& legal) {
    w.beginArray();
    for (bool value : legal) {
        w.boolean(value);
    }
    w.endArray();
}

// What a player sitting at the table sees: the seats, their stacks and bets, the pot,
// the board, whose turn it is and what they may do, and the hand so far.
template <int P>
void writeGame(JsonWriter& w, Replay<P>& replay, const std::array<bool, 5>& legal) {
    TableEngine<P>& engine = replay.engine;
    const auto& actor = engine.players[engine.action];
    const std::vector<std::string> names = seatNames(P);
    const float toCall = engine.bet - actor.bet;

    w.beginObject();
    w.key("config");
    writeConfig(w, replay.config);
    w.field("round", int(engine.round));
    w.field("street", std::string(STREET_NAMES[engine.round]));
    w.field("board", boardToString(replay.revealed()));
    w.field("pot", engine.pot);
    w.field("heads", engine.heads);
    w.field("blinds", std::string("1/2"));

    w.key("seats");
    w.beginArray();
    for (int seat = 0; seat < P; seat++) {
        const auto& player = engine.players[seat];
        w.beginObject();
        w.field("seat", seat);
        w.field("name", names[seat]);
        w.field("stack", player.chips);
        w.field("bet", player.bet);
        w.field("inHand", player.inHand);
        w.field("allIn", player.inHand && player.chips < 0.0005f);
        w.field("acting", seat == int(engine.action));
        w.endObject();
    }
    w.endArray();

    w.key("actor");
    w.beginObject();
    w.field("seat", int(engine.action));
    w.field("name", names[engine.action]);
    w.field("stack", actor.chips);
    w.field("bet", actor.bet);
    w.field("toCall", toCall);
    w.field("potAfterCall", engine.pot + toCall);
    w.field("spr", actor.spr);
    w.field("cpr", actor.cpr);
    w.endObject();

    w.key("legal");
    writeLegal(w, legal);
    w.key("options");
    w.beginArray();
    for (int option = 0; option < 5; option++) {
        w.beginObject();
        w.field("option", option);
        w.field("name", actionName(option, toCall, engine.round, engine.raiseHistory[engine.round]));
        w.field("size", std::string(option == 2 ? "0.5x pot" : option == 3 ? "1.5x pot" : ""));
        w.field("legal", legal[option]);
        if (legal[option] && option > 0) {
            const float to = engine.betTo(option);
            w.field("betTo", to);
            w.field("adds", to - actor.bet);
        } else {
            w.key("betTo");
            w.null();
            w.key("adds");
            w.null();
        }
        w.field("reason", legal[option] ? std::string() : illegalReason(engine, option));
        w.endObject();
    }
    w.endArray();

    w.key("history");
    w.beginArray();
    for (const HistoryEntry& entry : replay.history) {
        w.beginObject();
        if (entry.kind == HistoryEntry::Street) {
            w.field("kind", std::string("street"));
            w.field("round", entry.round);
            w.field("street", entry.name);
            w.field("board", entry.board);
        } else {
            w.field("kind", std::string(entry.kind == HistoryEntry::Post ? "post" : "action"));
            w.field("seat", entry.seat);
            w.field("name", entry.name);
            w.field("option", entry.option);
            w.field("round", entry.round);
            w.field("betTo", entry.betTo);
            w.field("adds", entry.adds);
            w.field("stack", entry.stack);
        }
        w.endObject();
    }
    w.endArray();
    w.endObject();
}

static std::string bitString(uint64_t value, int width) {
    std::string bits(width, '0');
    for (int i = 0; i < width; i++) {
        if (value >> (width - 1 - i) & 1) {
            bits[i] = '1';
        }
    }
    return bits;
}

static void writeKey(JsonWriter& w, const KeyLayout& layout) {
    w.beginObject();
    w.key("sharedKey");
    w.hex(layout.sharedKey);
    w.key("actionState");
    w.hex(layout.actionState);
    w.field("heroSeat", layout.heroSeat);
    w.field("villanSeat", layout.villanSeat);
    w.field("spr", layout.spr);
    w.field("cpr", layout.cpr);
    w.field("sharedBits", 58);
    w.key("fields");
    w.beginArray();
    for (const KeyField& field : layout.fields) {
        w.beginObject();
        w.field("name", field.name);
        w.field("label", field.label);
        w.field("shift", field.shift);
        w.field("width", field.width);
        w.field("value", int64_t(field.value));
        w.field("bits", bitString(field.value, field.width));
        w.field("meaning", field.meaning);
        w.field("zeroed", field.zeroed);
        w.field("shared", field.shared);
        w.key("parts");
        w.beginArray();
        for (const KeyPart& part : field.parts) {
            w.beginObject();
            w.field("name", part.name);
            w.field("value", part.value);
            w.field("meaning", part.meaning);
            w.field("zeroed", part.zeroed);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();
    w.endObject();
}

template <int P>
void writeSpot(JsonWriter& w, const SpotResult& spot, const ReplayDrops& drops) {
    w.beginObject();
    w.key("legal");
    writeLegal(w, spot.legal);
    w.field("invTemp", spot.invTemp);
    w.field("potAfterCall", spot.potAfterCall);
    w.field("records", spot.records);
    w.field("found", spot.found);
    w.field("missing", spot.missing);
    w.field("invalid", spot.invalid);
    w.field("rangeCombos", spot.rangeCombos);
    w.field("rangeWeight", spot.rangeWeight);
    w.field("rangeFoundWeight", spot.rangeFoundWeight);
    w.key("rangeMix");
    w.beginArray();
    for (double share : spot.rangeMix) {
        w.number(share);
    }
    w.endArray();
    w.field("rangeEv", spot.rangeEv);
    w.key("emptySeats");
    w.beginArray();
    for (int seat : spot.emptySeats) {
        w.integer(seat);
    }
    w.endArray();
    w.key("replayDrops");
    w.beginObject();
    w.field("decisions", drops.decisions);
    w.field("groups", drops.groups);
    w.field("combos", drops.combos);
    w.endObject();

    w.key("combos");
    w.beginArray();
    for (const SpotRow& row : spot.rows) {
        if (!row.valid) {
            w.null();
            continue;
        }
        w.beginObject();
        w.field("combo", cardToString(row.cards[0]) + cardToString(row.cards[1]));
        w.field("reach", row.reach);
        w.field("cluster", row.cluster);
        w.key("key");
        w.hex(row.key);
        w.field("found", row.found);
        w.field("clusterCombos", row.clusterCombos);
        if (row.found) {
            w.key("weights");
            w.beginArray();
            for (int j = 1; j < 5; j++) {
                // An illegal option's stored slot is meaningless, so it is absent.
                if (spot.legal[j]) {
                    w.number(row.weights[j - 1]);
                } else {
                    w.null();
                }
            }
            w.endArray();
            w.key("variances");
            w.beginArray();
            for (int j = 1; j < 5; j++) {
                if (spot.legal[j]) {
                    w.number(row.variances[j - 1]);
                } else {
                    w.null();
                }
            }
            w.endArray();
            w.key("frequencies");
            w.beginArray();
            for (double share : row.frequencies) {
                w.number(share);
            }
            w.endArray();
            w.key("evOptions");
            w.beginArray();
            for (int j = 0; j < 5; j++) {
                if (spot.legal[j]) {
                    w.number(row.evOptions[j]);
                } else {
                    w.null();
                }
            }
            w.endArray();
            w.field("ev", row.ev);
        }
        w.endObject();
    }
    w.endArray();
    w.endObject();
}

// One request at one player count. The replay is per thread: HandClusterer writes to
// its own scratch, and an engine is too large to build per request.
template <int P>
void answer(const Request& request, const StrategyStore& store, httplib::Response& response) {
    thread_local std::unique_ptr<Replay<P>> replay = std::make_unique<Replay<P>>();
    ReplayResult result = replay->run(request.config, request.actions, request.board, &store,
        request.invTemp);
    if (!result.ok) {
        json extra = json::object();
        if (result.actionIndex >= 0) {
            extra["actionIndex"] = result.actionIndex;
        }
        if (!result.reason.empty()) {
            extra["reason"] = result.reason;
        }
        if (result.hasLegal) {
            extra["legal"] = std::vector<bool>(result.legal.begin(), result.legal.end());
            extra["options"] = std::vector<std::string>(OPTION_NAMES, OPTION_NAMES + 5);
        }
        if (result.code == "needsBoard") {
            extra["street"] = STREET_NAMES[result.needRound];
            extra["cards"] = roundBoardCards(result.needRound);
        }
        if (result.code == "handEnded") {
            extra["endedBy"] = result.endedBy;
        }
        fail(response, 400, result.code, result.error, std::move(extra));
        return;
    }

    JsonWriter w;
    w.beginObject();
    if (request.include.game) {
        w.key("game");
        writeGame(w, *replay, result.legal);
    }
    if (request.include.key) {
        w.key("key");
        writeKey(w, decodeKey(*replay));
    }
    if (request.include.spot) {
        w.key("spot");
        writeSpot<P>(w, computeSpot(*replay, store, result.legal), replay->drops);
    }
    w.endObject();
    response.set_content(std::move(w.out), "application/json");
}

int main(int argc, char** argv) {
    try {
        ServerOptions options;
        for (int i = 1; i < argc; i++) {
            std::string flag = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error(flag + " needs a value");
                }
                return argv[++i];
            };
            if (flag == "--host") {
                options.host = next();
            } else if (flag == "--port") {
                options.port = std::stoi(next());
            } else if (flag == "--threads") {
                options.threads = std::stoi(next());
            } else if (flag == "--warm") {
                options.warm = true;
            } else {
                throw std::runtime_error("unrecognised flag " + flag);
            }
        }

        // Every table but handScores.bin, which only a showdown reads.
        ServerTables::load();

        StrategyRegistry registry;
        registry.warm = options.warm;
        registry.log = &std::cout;
        const StrategyConfig defaults;
        std::cout << "Default spec: " << defaults.spec() << "\n"
                  << "Available:";
        for (const StrategyConfig& config : availableStrategies()) {
            std::cout << " " << config.spec();
        }
        std::cout << std::endl;

        httplib::Server server;
        if (options.threads > 0) {
            server.new_task_queue = [&] {
                return new httplib::ThreadPool(size_t(options.threads));
            };
        }
        server.set_read_timeout(10, 0);
        server.set_write_timeout(30, 0);
        server.set_payload_max_length(1 << 20);

        server.Get("/health", [&](const httplib::Request&, httplib::Response& response) {
            json body;
            body["status"] = "ok";
            json loaded = json::array();
            for (const auto& [spec, records] : registry.loaded()) {
                loaded.push_back({{"spec", spec}, {"records", records}});
            }
            body["loaded"] = loaded;
            response.set_content(body.dump(), "application/json");
        });

        server.Get("/meta", [&](const httplib::Request&, httplib::Response& response) {
            json body;
            body["defaults"] = configJson(defaults);
            body["limits"] = {{"players", {3, 7}}, {"chipsMax", MAX_CHIPS}, {"chipsEven", true}};
            body["available"] = availableJson();
            body["blinds"] = {1, 2};
            body["invTemp"] = DEFAULT_INV_TEMP;
            body["invTempRange"] = {MIN_INV_TEMP, MAX_INV_TEMP};
            body["rangeFloor"] = RANGE_FLOOR;
            body["comboCount"] = COMBO_COUNT;
            body["options"] = std::vector<std::string>(OPTION_NAMES, OPTION_NAMES + 5);
            json seats = json::object();
            for (int players = 3; players <= 7; players++) {
                seats[std::to_string(players)] = seatNames(players);
            }
            body["seats"] = seats;
            body["cardEncoding"] = "rank * 4 + suit, rank 0 = ace descending to 12 = "
                "deuce, suits cdhs; combos are indexed i * (101 - i) / 2 + j - 1 for "
                "cards i < j";
            body["evUnits"] = "chips, relative to folding: a record's weight times the pot "
                "after calling";
            body["invTempUnits"] = "per pot: every policy is a softmax over each option's "
                "weight, its return as a share of the pot after calling";
            body["replay"] = "ranges are replayed through that same softmax at the request's "
                "invTemp and this server's RANGE_FLOOR; the default is the singleHand "
                "trainer's INV_TEMP";
            response.set_content(body.dump(2), "application/json");
        });

        server.Post("/strategy", [&](const httplib::Request& httpRequest,
                httplib::Response& response) {
            json body;
            try {
                body = json::parse(httpRequest.body);
            } catch (const std::exception& e) {
                fail(response, 400, "badRequest", std::string("malformed JSON body: ") + e.what());
                return;
            }
            if (!body.is_object()) {
                fail(response, 400, "badRequest", "the body must be a JSON object");
                return;
            }

            Request request;
            if (body.contains("actions") && !body["actions"].is_null()) {
                if (!body["actions"].is_array()) {
                    fail(response, 400, "badRequest",
                        "\"actions\" must be an array of option indices 0..4");
                    return;
                }
                if (body["actions"].size() > 256) {
                    fail(response, 400, "badRequest", "\"actions\" is longer than any hand can be");
                    return;
                }
                for (const auto& entry : body["actions"]) {
                    if (!entry.is_number_integer()) {
                        fail(response, 400, "badRequest", "\"actions\" must hold integers 0..4");
                        return;
                    }
                    request.actions.push_back(entry.get<int>());
                }
            }
            if (body.contains("board") && !body["board"].is_null()) {
                if (!body["board"].is_string()) {
                    fail(response, 400, "badRequest", "\"board\" must be a string like \"As7h2d\"");
                    return;
                }
                try {
                    request.board = parseBoard(body["board"].get<std::string>());
                } catch (const std::exception& e) {
                    fail(response, 400, "badBoard", e.what());
                    return;
                }
            }
            std::string error;
            if (!parseConfig(body, request.config, error)) {
                fail(response, 400, "badConfig", error);
                return;
            }
            if (!parseInclude(body, request.include, error)) {
                fail(response, 400, "badRequest", error);
                return;
            }
            if (body.contains("invTemp") && !body["invTemp"].is_null()) {
                const json& value = body["invTemp"];
                // A hair of slack, so a client's ln(2^k) need not match bit for bit.
                if (!value.is_number() || !std::isfinite(value.get<double>())
                        || value.get<double>() < MIN_INV_TEMP * (1 - 1e-9)
                        || value.get<double>() > MAX_INV_TEMP * (1 + 1e-9)) {
                    fail(response, 400, "badRequest", "\"invTemp\" must be a number from "
                        "ln(2^5) = " + std::to_string(MIN_INV_TEMP) + " to ln(2^80) = "
                        + std::to_string(MAX_INV_TEMP));
                    return;
                }
                request.invTemp = value.get<double>();
            }
            for (auto entry = body.begin(); entry != body.end(); ++entry) {
                const std::string& name = entry.key();
                if (name != "actions" && name != "board" && name != "config" && name != "include"
                        && name != "invTemp") {
                    fail(response, 400, "badRequest", "unknown field \"" + name
                        + "\" (valid: config, actions, board, invTemp, include)");
                    return;
                }
            }

            const std::string spec = request.config.spec();
            std::shared_ptr<const StrategyStore> store;
            try {
                store = registry.get(spec);
            } catch (const std::exception& e) {
                fail(response, 500, "strategyUnreadable",
                    "strategy_" + spec + ".bin exists but cannot be read: " + e.what());
                return;
            }
            if (store == nullptr) {
                fail(response, 404, "strategyMissing",
                    "no strategy has been trained for " + std::to_string(request.config.players)
                        + " players, " + std::to_string(request.config.chips) + " chips, metasBasic "
                        + (request.config.metasBasic ? "on" : "off") + ", memoryBasic "
                        + (request.config.memoryBasic ? "on" : "off")
                        + ": " + strategyPath(spec) + " does not exist",
                    {{"spec", spec}, {"path", strategyPath(spec)},
                        {"config", configJson(request.config)}, {"available", availableJson()}});
                return;
            }

            try {
                switch (request.config.players) {
                    case 3: answer<3>(request, *store, response); break;
                    case 4: answer<4>(request, *store, response); break;
                    case 5: answer<5>(request, *store, response); break;
                    case 6: answer<6>(request, *store, response); break;
                    case 7: answer<7>(request, *store, response); break;
                }
            } catch (const std::exception& e) {
                fail(response, 500, "internal", std::string("replay failed: ") + e.what());
            }
        });

        server.set_exception_handler([](const httplib::Request&, httplib::Response& response,
                std::exception_ptr ep) {
            std::string message = "internal error";
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                message = e.what();
            } catch (...) {
            }
            fail(response, 500, "internal", message);
        });

        std::cout << "Listening on " << options.host << ":" << options.port << std::endl;
        if (!server.listen(options.host, options.port)) {
            throw std::runtime_error("failed to bind " + options.host + ":"
                + std::to_string(options.port));
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
