// Firefly strategy API.
//
//   build/server [--host H] [--port P] [--strategy PATH] [--index PATH]
//                [--threads N] [--warm]
//
// Must be run from the repository root: engine.h resolves the handAnalysis tables
// through relative paths.
//
// POST /strategy  {"actions": [0,0,3,1], "board": "As7h2d", "invTemp": 11.0904}
//   -> the weights, frequencies and visits of all 1326 hole card combinations at
//      the node that action list reaches, for the player to act.
// GET  /health
// GET  /meta

#define CPPHTTPLIB_ZLIB_SUPPORT

#include "solver/config.h"
#include "server/replay.h"
#include "server/sweep.h"
#include "server/strategyStore.h"
#include "server/include/httplib.h"
#include "server/include/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

using json = nlohmann::json;

// The response is 1326 rows of ten numbers. A generic serialiser is the dominant
// cost of an otherwise few millisecond request, so it is written straight into one
// preallocated buffer with to_chars.
struct JsonWriter {
    std::string out;

    JsonWriter() { out.reserve(1 << 19); }

    void raw(const char* text) { out += text; }
    void raw(const std::string& text) { out += text; }

    void number(double value) {
        if (!std::isfinite(value)) {
            // Neither inf nor nan is valid JSON. A finite file never produces
            // them, but a corrupt one should not produce an unparseable body.
            out += "null";
            return;
        }
        char buffer[40];
        auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out.append(buffer, end);
    }

    void number(uint64_t value) {
        char buffer[24];
        auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out.append(buffer, end);
    }

    void number(int value) { number(uint64_t(value < 0 ? 0 : value)); }

    // Keys are hex strings: a uint64 exceeds JSON's safe integer range and a
    // JavaScript client would silently round it.
    void hexKey(uint64_t value) {
        char buffer[20] = "\"0x";
        auto [end, ec] = std::to_chars(buffer + 3, buffer + 19, value, 16);
        *end = '"';
        out.append(buffer, end + 1);
    }

    void string(const std::string& text) {
        out += '"';
        out += text;
        out += '"';
    }

    void key(const char* name) {
        out += '"';
        out += name;
        out += "\":";
    }
};

struct ServerOptions {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string strategyPath;
    std::string indexPath;
    int threads = 0;
    bool warm = false;
};

static void writeBoolArray(JsonWriter& writer, const std::array<bool, 5>& values) {
    writer.raw("[");
    for (int j = 0; j < 5; j++) {
        writer.raw(values[j] ? "true" : "false");
        if (j < 4) {
            writer.raw(",");
        }
    }
    writer.raw("]");
}

// Which sections the caller asked for. Everything is off unless named, so a
// request that turns nothing on returns {} - which still means the action list
// replayed legally, making it a cheap validity check.
struct IncludeMask {
    bool node = false;
    bool transition = false;
    bool keyComponents = false;
    SweepFields combos;

    bool anything() const {
        return node || transition || keyComponents || combos.any();
    }
};

// Writes a section separator only from the second section onward, so the caller
// need not know which sections precede it.
struct SectionWriter {
    JsonWriter& writer;
    bool written = false;

    void begin(const char* name) {
        if (written) {
            writer.raw(",");
        }
        written = true;
        writer.key(name);
    }
};

static void writeIntArray(JsonWriter& writer, const std::array<int, 4>& values) {
    writer.raw("[");
    for (int i = 0; i < 4; i++) {
        writer.number(uint64_t(values[i]));
        if (i < 3) {
            writer.raw(",");
        }
    }
    writer.raw("]");
}

// The abstraction's view of the node: what the key identifies, simplified.
static void writeNode(JsonWriter& writer, const Engine& engine,
        const std::vector<int>& board, const std::array<bool, 5>& legal,
        double invTemp) {
    writer.raw("{");
    writer.key("spec");    writer.string(fireflySpec());
    writer.raw(",");
    writer.key("round");   writer.number(uint64_t(engine.round));
    writer.raw(",");
    writer.key("street");  writer.string(engine.round == 0 ? "preflop"
        : engine.round == 1 ? "flop" : engine.round == 2 ? "turn" : "river");
    writer.raw(",");
    writer.key("seat");    writer.number(uint64_t(engine.action));
    writer.raw(",");
    writer.key("heads");   writer.number(uint64_t(engine.heads));
    writer.raw(",");
    writer.key("board");
    {
        std::string text;
        for (int card : board) {
            text += cardToString(card);
        }
        writer.string(text);
    }
    writer.raw(",");
    // legal lives here because it is what the rows' nulls and zeros are keyed to.
    writer.key("legal");   writeBoolArray(writer, legal);
    writer.raw(",");
    writer.key("options");
    writer.raw("[\"fold\",\"call\",\"raise 0.5x pot\",\"raise 1.5x pot\",\"all in\"]");
    writer.raw(",");
    writer.key("invTemp"); writer.number(invTemp);
    writer.raw(",");
    writer.key("weightUnits");
    writer.string("stack change divided by the pot after calling, measured against "
        "folding, whose value is exactly 0");
    writer.raw("}");
}

// Exact chips, for a UI applying one action and rendering the next decision. Only
// the two seats that changed; the rest of the table accumulates on the client.
static void writeTransition(JsonWriter& writer, const Engine& engine,
        const ReplayResult& replayed, const std::array<bool, 5>& legal) {
    const Engine::Player& actor = engine.players[engine.action];
    float toCall = engine.bet - actor.bet;

    writer.raw("{");
    writer.key("pot"); writer.number(double(engine.pot));
    writer.raw(",");
    writer.key("lastAction");
    if (replayed.lastSeat < 0) {
        // No action was submitted, so nothing preceded this node.
        writer.raw("null");
    } else {
        const Engine::Player& last = engine.players[replayed.lastSeat];
        writer.raw("{");
        writer.key("seat");   writer.number(uint64_t(replayed.lastSeat));
        writer.raw(",");
        writer.key("option"); writer.number(uint64_t(replayed.lastOption));
        writer.raw(",");
        writer.key("chips");  writer.number(double(last.chips));
        writer.raw(",");
        // If the action closed the street, newRound() has already zeroed every
        // live player's bet. The chips are in the pot and the stack is in chips,
        // so this reading 0 is correct - but a UI rendering bet as "chips in
        // front" needs to notice the street rolled over.
        writer.key("bet");    writer.number(double(last.bet));
        writer.raw(",");
        writer.key("inHand"); writer.raw(last.inHand ? "true" : "false");
        writer.raw("}");
    }
    writer.raw(",");
    writer.key("actor");
    writer.raw("{");
    writer.key("seat");         writer.number(uint64_t(engine.action));
    writer.raw(",");
    writer.key("chips");        writer.number(double(actor.chips));
    writer.raw(",");
    writer.key("bet");          writer.number(double(actor.bet));
    writer.raw(",");
    writer.key("toCall");       writer.number(double(toCall));
    writer.raw(",");
    // The weight denominator, repeated here because it is exact.
    writer.key("potAfterCall"); writer.number(double(engine.pot + toCall));
    writer.raw(",");
    writer.key("spr");          writer.number(double(actor.spr));
    writer.raw(",");
    writer.key("cpr");          writer.number(double(actor.cpr));
    writer.raw(",");
    writer.key("legal");        writeBoolArray(writer, legal);
    writer.raw("}}");
}

static void writeKeyComponents(JsonWriter& writer, const KeyComponents& parts) {
    writer.raw("{");
    writer.key("roundHeadsBasic");      writer.number(uint64_t(parts.roundHeadsBasic));
    writer.raw(",");
    writer.key("headsBasic");           writer.number(uint64_t(parts.headsBasic));
    writer.raw(",");
    writer.key("actionPositionBucket"); writer.number(uint64_t(parts.actionPositionBucket));
    writer.raw(",");
    writer.key("sprCprBucket");         writer.number(uint64_t(parts.sprCprBucket));
    writer.raw(",");
    writer.key("sprCprKind");           writer.string(parts.sprCprKind);
    writer.raw(",");
    // Preflop the board field carries the 169 combo index, which is hand
    // dependent, so there is no shared value to report.
    writer.key("boardField");
    if (parts.hasBoardField) {
        writer.number(uint64_t(parts.boardField));
    } else {
        writer.raw("null");
    }
    writer.raw(",");
    writer.key("boardClass");
    if (parts.hasBoardField) {
        writer.number(uint64_t(parts.boardClass));
    } else {
        writer.raw("null");
    }
    writer.raw(",");
    // The four per street counts, in place of the colex rank the key packs.
    writer.key("raiseHistory");       writeIntArray(writer, parts.raiseHistory);
    writer.raw(",");
    writer.key("playerLastActions");  writeIntArray(writer, parts.playerLastActions);
    writer.raw(",");
    writer.key("villanLastActions");  writeIntArray(writer, parts.villanLastActions);
    writer.raw(",");
    writer.key("playerPositionBasic"); writer.number(uint64_t(parts.playerPositionBasic));
    writer.raw(",");
    writer.key("villanPositionBasic"); writer.number(uint64_t(parts.villanPositionBasic));
    writer.raw(",");
    writer.key("villanPosition");      writer.number(uint64_t(parts.villanPosition));
    writer.raw(",");
    writer.key("raiseSizeOverZero");   writer.number(uint64_t(parts.raiseSizeOverZero));
    writer.raw(",");
    writer.key("raiseStrength");       writer.number(uint64_t(parts.raiseStrength));
    writer.raw(",");
    writer.key("hiddenRaiseSizeOverZero");
    writer.number(uint64_t(parts.hiddenRaiseSizeOverZero));
    writer.raw(",");
    writer.key("hiddenRaiseStrength"); writer.number(uint64_t(parts.hiddenRaiseStrength));
    writer.raw(",");
    writer.key("raisesFaced");         writer.number(uint64_t(parts.raisesFaced));
    writer.raw(",");
    writer.key("relativePosition");    writer.number(uint64_t(parts.relativePosition));
    writer.raw(",");
    writer.key("playerCallsOverOne");  writer.raw(parts.playerCallsOverOne ? "true" : "false");
    writer.raw(",");
    writer.key("calls");               writer.number(uint64_t(parts.calls));
    writer.raw("}");
}

static void writeCombos(JsonWriter& writer, const SweepResult& swept,
        const SweepFields& fields) {
    // found is emitted whenever a record backed field is present, because those
    // fields are meaningless without it. It is not separately toggleable.
    const bool emitFound = fields.needsStore();
    writer.raw("[");
    for (int i = 0; i < COMBO_COUNT; i++) {
        const ComboRow& row = swept.rows[i];
        if (i > 0) {
            writer.raw(",");
        }
        if (!row.valid) {
            // The combination clashes with a board card, so it cannot be held.
            writer.raw("null");
            continue;
        }
        writer.raw("{");
        writer.key("combo");
        writer.string(cardToString(row.cards[0]) + cardToString(row.cards[1]));
        if (!row.metaFound) {
            writer.raw(",");
            writer.key("found");     writer.raw("false");
            writer.raw(",");
            writer.key("metaFound"); writer.raw("false");
            writer.raw("}");
            continue;
        }
        if (fields.handMeta) {
            writer.raw(",");
            writer.key("handMeta"); writer.number(uint64_t(row.handMeta));
        }
        if (fields.key) {
            writer.raw(",");
            writer.key("key"); writer.hexKey(row.key);
        }
        if (emitFound) {
            writer.raw(",");
            writer.key("found"); writer.raw(row.found ? "true" : "false");
        }
        if (row.found && fields.weights) {
            writer.raw(",");
            writer.key("weights");
            writer.raw("[");
            for (int j = 1; j < 5; j++) {
                // An illegal option's stored slot holds whatever was in memory,
                // so it is reported as absent rather than as a number.
                if (swept.legal[j]) {
                    writer.number(row.weights[j - 1]);
                } else {
                    writer.raw("null");
                }
                if (j < 4) {
                    writer.raw(",");
                }
            }
            writer.raw("]");
        }
        if (row.found && fields.frequencies) {
            writer.raw(",");
            writer.key("frequencies");
            writer.raw("[");
            for (int j = 0; j < 5; j++) {
                writer.number(row.frequencies[j]);
                if (j < 4) {
                    writer.raw(",");
                }
            }
            writer.raw("]");
        }
        if (row.found && fields.visits) {
            writer.raw(",");
            // The per holding estimate, plus the two numbers it is built from so
            // it can be checked and re-aggregated.
            writer.key("visits");     writer.number(row.visits);
            writer.raw(",");
            writer.key("nodeVisits"); writer.number(uint64_t(row.nodeVisits));
            writer.raw(",");
            writer.key("metaCombos"); writer.number(uint64_t(row.metaCombos));
        }
        writer.raw("}");
    }
    writer.raw("]");
}

static std::string buildResponse(const Engine& engine, const std::vector<int>& board,
        const ReplayResult& replayed, const SweepResult& swept,
        const std::array<bool, 5>& legal, double invTemp, const IncludeMask& include) {
    JsonWriter writer;
    SectionWriter sections{writer};
    writer.raw("{");

    if (include.node) {
        sections.begin("node");
        writeNode(writer, engine, board, legal, invTemp);
    }
    if (include.transition) {
        sections.begin("transition");
        writeTransition(writer, engine, replayed, legal);
    }
    if (include.keyComponents) {
        sections.begin("keyComponents");
        writeKeyComponents(writer, keyComponents(engine));
    }
    if (include.combos.counts) {
        sections.begin("counts");
        writer.raw("{");
        writer.key("found");       writer.number(uint64_t(swept.found));
        writer.raw(",");
        writer.key("missing");     writer.number(uint64_t(swept.missing));
        writer.raw(",");
        writer.key("invalid");     writer.number(uint64_t(swept.invalid));
        writer.raw(",");
        writer.key("metaMissing"); writer.number(uint64_t(swept.metaMissing));
        writer.raw("}");
    }
    if (include.combos.weights || include.combos.frequencies || include.combos.visits
            || include.combos.key || include.combos.handMeta) {
        sections.begin("combos");
        writeCombos(writer, swept, include.combos);
    }

    writer.raw("}");
    return std::move(writer.out);
}

// Parses the include block. Each section accepts true, false, or - for combos - an
// object naming individual fields. An unknown key is a 400 rather than a silent
// omission: a typo would otherwise return {} and read as a working request that
// found nothing.
static bool parseInclude(const json& body, IncludeMask& include, std::string& error) {
    if (!body.contains("include") || body["include"].is_null()) {
        return true;
    }
    const json& node = body["include"];
    if (!node.is_object()) {
        error = "\"include\" must be an object, for example "
            "{\"node\": true, \"combos\": {\"weights\": true}}";
        return false;
    }

    auto readFlag = [&](const json& value, const char* name, bool& out) {
        if (!value.is_boolean()) {
            error = std::string("\"include.") + name + "\" must be true or false";
            return false;
        }
        out = value.get<bool>();
        return true;
    };

    for (auto entry = node.begin(); entry != node.end(); ++entry) {
        const std::string& name = entry.key();
        if (name == "node") {
            if (!readFlag(entry.value(), "node", include.node)) { return false; }
        } else if (name == "transition") {
            if (!readFlag(entry.value(), "transition", include.transition)) { return false; }
        } else if (name == "keyComponents") {
            if (!readFlag(entry.value(), "keyComponents", include.keyComponents)) { return false; }
        } else if (name == "combos") {
            const json& combos = entry.value();
            if (combos.is_boolean()) {
                // Shorthand: true turns on every combo field.
                const bool all = combos.get<bool>();
                include.combos = SweepFields{all, all, all, all, all, all};
            } else if (combos.is_object()) {
                for (auto field = combos.begin(); field != combos.end(); ++field) {
                    const std::string& key = field.key();
                    bool* target = key == "weights"     ? &include.combos.weights
                                 : key == "frequencies" ? &include.combos.frequencies
                                 : key == "visits"      ? &include.combos.visits
                                 : key == "key"         ? &include.combos.key
                                 : key == "handMeta"    ? &include.combos.handMeta
                                 : key == "counts"      ? &include.combos.counts
                                 : nullptr;
                    if (target == nullptr) {
                        error = "unknown field \"" + key + "\" in \"include.combos\" "
                            "(valid: weights, frequencies, visits, key, handMeta, counts)";
                        return false;
                    }
                    if (!readFlag(field.value(), ("combos." + key).c_str(), *target)) {
                        return false;
                    }
                }
            } else {
                error = "\"include.combos\" must be a boolean or an object";
                return false;
            }
        } else {
            error = "unknown section \"" + name + "\" in \"include\" "
                "(valid: node, transition, keyComponents, combos)";
            return false;
        }
    }
    return true;
}

static void fail(httplib::Response& response, int status, const std::string& message,
        int actionIndex = -1, const std::array<bool, 5>* legal = nullptr,
        const std::string& reason = "") {
    json body;
    body["error"] = message;
    // Names the restriction that turned the option off. The message says only that
    // an option is illegal; without this a caller cannot tell whether it hit the
    // limping rule, a call cap, or an SPR threshold.
    if (!reason.empty()) {
        body["reason"] = reason;
    }
    if (actionIndex >= 0) {
        body["actionIndex"] = actionIndex;
    }
    if (legal != nullptr) {
        body["legal"] = std::vector<bool>(legal->begin(), legal->end());
        body["options"] = std::vector<std::string>(OPTION_NAMES, OPTION_NAMES + 5);
    }
    response.status = status;
    response.set_content(body.dump(), "application/json");
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
            } else if (flag == "--strategy") {
                options.strategyPath = next();
            } else if (flag == "--index") {
                options.indexPath = next();
            } else if (flag == "--threads") {
                options.threads = std::stoi(next());
            } else if (flag == "--warm") {
                options.warm = true;
            } else {
                throw std::runtime_error("unrecognised flag " + flag);
            }
        }
        if (options.strategyPath.empty()) {
            options.strategyPath = defaultStrategyPath();
        }
        if (options.indexPath.empty()) {
            options.indexPath = defaultIndexPath();
        }

        // Before any Engine exists: the constructor also calls loadTables, and the
        // once_flag means the first caller decides whether the 535 MB showdown
        // table is read. Nothing here ever reaches showdown.
        Engine::loadTables(false);

        StrategyStore store;
        store.open(options.strategyPath, options.indexPath);
        std::cout << "Spec: " << fireflySpec() << "\n"
                  << "Strategy: " << options.strategyPath << " (" << store.records
                  << " records)\n"
                  << "Mapped: " << (store.mappedBytes() / (1024.0 * 1024.0)) << " MB"
                  << std::endl;
        if (options.warm) {
            uint64_t sink = store.warm();
            std::cout << "Warmed the mappings (" << (sink & 1) << ")" << std::endl;
        }

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
            body["spec"] = fireflySpec();
            body["records"] = store.records;
            body["mappedBytes"] = store.mappedBytes();
            response.set_content(body.dump(), "application/json");
        });

        server.Get("/meta", [&](const httplib::Request&, httplib::Response& response) {
            json body;
            body["spec"] = fireflySpec();
            body["playerCount"] = PLAYER_COUNT;
            body["startingChips"] = CHIP_COUNT;
            body["blinds"] = {1, 2};
            body["metasBasic"] = METAS_BASIC;
            body["memoryBasic"] = MEMORY_BASIC;
            body["records"] = store.records;
            body["defaultInvTemp"] = TERMINAL_INV_TEMP;
            body["comboCount"] = COMBO_COUNT;
            body["options"] = std::vector<std::string>(OPTION_NAMES, OPTION_NAMES + 5);
            body["seats"] = {"BTN", "SB", "BB", "UTG", "UTG+1", "MP", "CO"};
            body["cardEncoding"] = "rank * 4 + suit, rank 0 = ace descending to 12 = "
                "deuce, suits cdhs; combos are indexed i * (101 - i) / 2 + j - 1 for "
                "cards i < j";
            body["weightUnits"] = "stack change divided by the pot after calling, "
                "measured against folding, whose value is exactly 0";
            response.set_content(body.dump(2), "application/json");
        });

        server.Post("/strategy", [&](const httplib::Request& request,
                httplib::Response& response) {
            json body;
            try {
                body = json::parse(request.body);
            } catch (const std::exception& e) {
                fail(response, 400, std::string("malformed JSON body: ") + e.what());
                return;
            }
            if (!body.is_object()) {
                fail(response, 400, "the body must be a JSON object");
                return;
            }

            std::vector<int> actions;
            if (body.contains("actions") && !body["actions"].is_null()) {
                if (!body["actions"].is_array()) {
                    fail(response, 400, "\"actions\" must be an array of option indices 0..4");
                    return;
                }
                if (body["actions"].size() > 256) {
                    fail(response, 400, "\"actions\" is longer than any hand can be");
                    return;
                }
                for (const auto& entry : body["actions"]) {
                    if (!entry.is_number_integer()) {
                        fail(response, 400, "\"actions\" must hold integers 0..4");
                        return;
                    }
                    actions.push_back(entry.get<int>());
                }
            }

            std::vector<int> board;
            if (body.contains("board") && !body["board"].is_null()) {
                if (!body["board"].is_string()) {
                    fail(response, 400, "\"board\" must be a string like \"As7h2d\"");
                    return;
                }
                try {
                    board = parseBoard(body["board"].get<std::string>());
                } catch (const std::exception& e) {
                    fail(response, 400, e.what());
                    return;
                }
            }

            double invTemp = TERMINAL_INV_TEMP;
            if (body.contains("invTemp") && !body["invTemp"].is_null()) {
                if (!body["invTemp"].is_number()) {
                    fail(response, 400, "\"invTemp\" must be a number");
                    return;
                }
                invTemp = body["invTemp"].get<double>();
                if (!std::isfinite(invTemp) || invTemp < 0.0 || invTemp > 1e6) {
                    fail(response, 400, "\"invTemp\" must be finite and in [0, 1e6]");
                    return;
                }
            }

            IncludeMask include;
            {
                std::string error;
                if (!parseInclude(body, include, error)) {
                    fail(response, 400, error);
                    return;
                }
            }

            // One Engine per worker thread: HandClusterer::classifyHand writes to
            // its own scratch arrays, so it cannot be shared.
            thread_local Replay replay;
            ReplayResult result;
            try {
                result = replay.run(actions, board);
            } catch (const std::exception& e) {
                fail(response, 500, std::string("replay failed: ") + e.what());
                return;
            }
            if (!result.ok) {
                fail(response, 400, result.error, result.actionIndex,
                    result.hasLegal ? &result.legal : nullptr, result.reason);
                return;
            }

            // Nothing was asked for. The replay still validated the action list, so
            // a 200 with an empty body means the line is legal.
            if (!include.anything()) {
                response.set_content("{}", "application/json");
                return;
            }

            // The sweep is 1326 classifications; skip it entirely unless a combo
            // field needs it. A transition-only request is then just a replay.
            SweepResult swept;
            if (include.combos.any()) {
                // sweep() calls getState() then getOptions() itself and stores
                // the result in swept.legal.
                swept = sweep(replay.engine, replay.board, store, invTemp,
                    include.combos);
            } else {
                // getState() assigns spr, cpr and actionPosition, which both
                // getOptions() and keyComponents() read. sweep() does this itself.
                replay.engine.getState(METAS_BASIC, MEMORY_BASIC);
                swept.legal = replay.engine.getOptions();
            }

            response.set_content(
                buildResponse(replay.engine, replay.board, result, swept,
                    swept.legal, invTemp, include),
                "application/json");
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
            json body;
            body["error"] = message;
            response.status = 500;
            response.set_content(body.dump(), "application/json");
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
