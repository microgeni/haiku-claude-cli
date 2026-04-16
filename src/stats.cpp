#include "stats.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "paths.h"

using json = nlohmann::json;

namespace stats {

namespace {

std::string stats_path() {
    return paths::config_dir() + "/stats.json";
}

// Default stats blob returned when the file is missing or unreadable.
// Kept as a free function so load() doesn't have to repeat the literal
// twice (missing-file and parse-error cases both want the same shape).
json fresh() {
    return {
        {"first_session", ""},
        {"sessions",      0},
        {"turns",         0},
        {"input_tokens",  0},
        {"output_tokens", 0},
        {"tool_calls",    json::object()},
    };
}

json load() {
    std::ifstream f(stats_path());
    if (!f.is_open()) return fresh();
    try {
        return json::parse(f);
    } catch (const json::exception&) {
        return fresh();
    }
}

void save(const json& s) {
    paths::mkdir_p(paths::config_dir());
    std::ofstream f(stats_path());
    if (f.is_open()) {
        f << s.dump(2) << "\n";
    }
}

// Insert thousands separators into a non-negative integer so
// "654229" renders as "654,229" in the stats table. Keeps the
// numbers scannable — the difference between 6,600 and 66,000
// saved tokens is the whole story of /stats.
std::string thousands(long n) {
    if (n < 0) return "-" + thousands(-n);
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(i, ",");
    }
    return s;
}

} // namespace

void record_session() {
    json s = load();
    s["sessions"] = s.value("sessions", 0) + 1;
    if (s.value("first_session", std::string{}).empty()) {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char date[16];
        std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);
        s["first_session"] = date;
    }
    save(s);
}

void record_turn(int input_tokens, int output_tokens) {
    json s = load();
    s["turns"]         = s.value("turns", 0) + 1;
    s["input_tokens"]  = s.value("input_tokens", 0) + input_tokens;
    s["output_tokens"] = s.value("output_tokens", 0) + output_tokens;
    save(s);
}

void record_tool(const std::string& tool_name, int result_bytes,
                 long saved_bytes) {
    json s = load();
    auto& tc = s["tool_calls"];
    if (!tc.is_object()) tc = json::object();
    if (!tc.contains(tool_name)) {
        tc[tool_name] = {{"count", 0}, {"bytes", 0}, {"saved_bytes", 0}};
    }
    tc[tool_name]["count"] = tc[tool_name].value("count", 0) + 1;
    tc[tool_name]["bytes"] = tc[tool_name].value("bytes", 0) + result_bytes;
    if (saved_bytes > 0) {
        tc[tool_name]["saved_bytes"] =
            tc[tool_name].value("saved_bytes", static_cast<long>(0)) + saved_bytes;
    }
    save(s);
}

std::string format_display() {
    const json s = load();
    const std::string since = s.value("first_session", std::string{"(unknown)"});
    const int  sessions = s.value("sessions", 0);
    const int  turns    = s.value("turns", 0);
    const long in_tok   = s.value("input_tokens",  static_cast<long>(0));
    const long out_tok  = s.value("output_tokens", static_cast<long>(0));

    // Ballpark lifetime cost using Sonnet rates ($3 / M input,
    // $15 / M output). Stats are cross-model but Sonnet is the
    // default and gives an order-of-magnitude number.
    const double est_cost = (in_tok  / 1'000'000.0) * 3.0
                          + (out_tok / 1'000'000.0) * 15.0;

    // BFS counters — computed first so the summary block can
    // lead with the savings number.
    long read_attr_saved = 0, read_attr_used = 0;
    int  read_attr_calls = 0;
    long query_saved     = 0, query_used    = 0;
    int  query_calls     = 0;
    if (s.contains("tool_calls") && s["tool_calls"].is_object()) {
        const auto& tc = s["tool_calls"];
        if (tc.contains("ReadAttr")) {
            read_attr_saved = tc["ReadAttr"].value("saved_bytes", static_cast<long>(0));
            read_attr_used  = tc["ReadAttr"].value("bytes",       static_cast<long>(0));
            read_attr_calls = tc["ReadAttr"].value("count",       0);
        }
        if (tc.contains("Query")) {
            query_saved = tc["Query"].value("saved_bytes", static_cast<long>(0));
            query_used  = tc["Query"].value("bytes",       static_cast<long>(0));
            query_calls = tc["Query"].value("count",       0);
        }
    }
    const long total_saved_bytes = read_attr_saved + query_saved;
    const long total_used_bytes  = read_attr_used  + query_used;
    const long saved_tokens      = total_saved_bytes / 4;
    const long used_tokens       = total_used_bytes  / 4;
    const long full_tokens       = saved_tokens + used_tokens;
    const int  bfs_pct           = full_tokens > 0
        ? static_cast<int>((saved_tokens * 100) / full_tokens)
        : 0;
    const double bfs_cost_saved  = (saved_tokens / 1'000'000.0) * 3.0;

    char buf[768];
    std::string out;

    out += "haiku-claude-cli lifetime stats";
    if (!since.empty() && since != "(unknown)") {
        out += " (since " + since + ")";
    }
    out += "\n\n";

    std::snprintf(buf, sizeof(buf),
        "  Sessions:  %d\n"
        "  Turns:     %d\n"
        "  Input:     %s tokens\n"
        "  Output:    %s tokens\n"
        "  Est cost:  $%.2f  (Sonnet rates)\n",
        sessions, turns,
        thousands(in_tok).c_str(),
        thousands(out_tok).c_str(),
        est_cost);
    out += buf;

    // ── BFS showcase block ───────────────────────────────
    out += "\n";
    out += "  \xE2\x94\x83 BFS — the Haiku advantage\n";
    out += "  \xE2\x94\x83\n";
    if (saved_tokens > 0) {
        std::snprintf(buf, sizeof(buf),
            "  \xE2\x94\x83  Saved %s tokens  (%d%% of full-read cost)\n"
            "  \xE2\x94\x83  Cost avoided: $%.4f\n"
            "  \xE2\x94\x83\n"
            "  \xE2\x94\x83  %d BFS calls  (%d ReadAttr + %d Query)\n"
            "  \xE2\x94\x83  Tokens they used:     %8s\n"
            "  \xE2\x94\x83  Tokens they avoided:  %8s\n",
            thousands(saved_tokens).c_str(), bfs_pct, bfs_cost_saved,
            read_attr_calls + query_calls,
            read_attr_calls, query_calls,
            thousands(used_tokens).c_str(),
            thousands(saved_tokens).c_str());
    } else {
        std::snprintf(buf, sizeof(buf),
            "  \xE2\x94\x83  Cache empty — builds itself as Claude\n"
            "  \xE2\x94\x83  reads files this session.\n");
    }
    out += buf;
    out += "\n";

    // ── Tool call table ──────────────────────────────────
    if (s.contains("tool_calls") && s["tool_calls"].is_object()) {
        out += "  Tool calls (lifetime):\n";
        for (const auto& [name, val] : s["tool_calls"].items()) {
            const int  count = val.value("count", 0);
            const bool is_bfs = (name == "ReadAttr"  || name == "Query"
                              || name == "WriteAttr" || name == "IndexAttr");
            std::snprintf(buf, sizeof(buf),
                "    %-14s %5d calls%s\n",
                name.c_str(), count,
                is_bfs ? "  [BFS]" : "");
            out += buf;
        }
    }

    return out;
}

} // namespace stats
