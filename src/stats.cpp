#include "stats.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "paths.h"

using json = nlohmann::json;

namespace stats {

namespace {

std::string stats_path() {
	return paths::ConfigDir() + "/stats.json";
}

std::string stats_tmp_path() {
	return paths::ConfigDir() + "/stats.json.tmp";
}

std::string stats_bak_path() {
	return paths::ConfigDir() + "/stats.json.bak";
}

// Default stats blob returned when the file is missing or unreadable.
// Kept as a free function so load() doesn't have to repeat the literal
// twice (missing-file and parse-error cases both want the same shape).
json fresh() {
	return {
		{"first_session", ""},
		{"sessions",      0LL},
		{"turns",         0LL},
		{"input_tokens",  0LL},
		{"output_tokens", 0LL},
		{"tool_calls",    json::object()},
	};
}

// Parse a JSON file at `path`. Returns an empty optional on any error.
std::optional<json> parse_file(const std::string& path) {
	std::ifstream f(path);
	if (!f.is_open()) return std::nullopt;
	try {
		return json::parse(f);
	} catch (const json::exception&) {
		return std::nullopt;
	}
}

// Migrate a single tool_calls entry: rename the legacy "fSavedbytes"
// key (written by an early buggy version) to "saved_bytes" so the
// BFS savings counter survives across the schema change.
void migrate_tool_entry(json& entry) {
    if (entry.contains("fSavedbytes") && !entry.contains("saved_bytes")) {
        entry["saved_bytes"] = entry["fSavedbytes"];
        entry.erase("fSavedbytes");
    }
}

json load() {
	// Helper: run schema migrations on a freshly parsed stats blob.
	auto migrate = [](json j) -> json {
		if (j.contains("tool_calls") && j["tool_calls"].is_object())
			for (auto& [name, val] : j["tool_calls"].items())
				migrate_tool_entry(val);
		return j;
	};

	// 1. Try the live file.
	if (auto j = parse_file(stats_path())) return migrate(*j);

	// 2. Live file missing or corrupt — try the last-good backup.
	if (auto j = parse_file(stats_bak_path())) {
		// Restore it so future crashes recover the same way.
		std::ifstream src(stats_bak_path(), std::ios::binary);
		std::ofstream dst(stats_path(),     std::ios::binary);
		if (src && dst) dst << src.rdbuf();
		return migrate(*j);
	}

	// 3. Nothing salvageable — start fresh.
	return fresh();
}

// Atomic save: write to a .tmp file then rename over the live path.
// rename(2) is guaranteed atomic on POSIX so a crash mid-write leaves
// the previous live file intact.  After a successful rename we also
// copy the live file to .bak so there is always a last-known-good
// backup one generation behind the live data.
void save(const json& s) {
	paths::MkdirP(paths::ConfigDir());
	const std::string tmp  = stats_tmp_path();
	const std::string live = stats_path();
	const std::string bak  = stats_bak_path();

	// Write to .tmp first.
	{
		std::ofstream f(tmp);
		if (!f.is_open()) return;
		f << s.dump(2) << "\n";
		if (!f.good()) { std::remove(tmp.c_str()); return; }
	}

	// Promote .tmp → live (atomic on POSIX).
	if (std::rename(tmp.c_str(), live.c_str()) != 0) {
		std::remove(tmp.c_str());
		return;
	}

	// Copy live → .bak (best-effort; never clobber live on failure).
	{
		std::ifstream src(live, std::ios::binary);
		std::ofstream dst(bak,  std::ios::binary);
		if (src && dst) dst << src.rdbuf();
	}
}

// Insert thousands separators into a non-negative integer so
// "654229" renders as "654,229" in the stats table. Keeps the
// numbers scannable — the difference between 6,600 and 66,000
// saved tokens is the whole story of /stats.
std::string thousands(long long n) {
	if (n < 0) return "-" + thousands(-n);
	std::string s = std::to_string(n);
	for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
		s.insert(i, ",");
	}
	return s;
}

} // namespace

void RecordSession() {
	json s = load();
	s["sessions"] = s.value("sessions", 0LL) + 1LL;
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

void RecordTurn(int input_tokens, int output_tokens,
				int cache_read_tokens, int cache_write_tokens) {
	json s = load();
	s["turns"]               = s.value("turns",               0LL) + 1LL;
	s["input_tokens"]        = s.value("input_tokens",        0LL) + static_cast<long long>(input_tokens);
	s["output_tokens"]       = s.value("output_tokens",       0LL) + static_cast<long long>(output_tokens);
	s["cache_read_tokens"]   = s.value("cache_read_tokens",   0LL) + static_cast<long long>(cache_read_tokens);
	s["cache_write_tokens"]  = s.value("cache_write_tokens",  0LL) + static_cast<long long>(cache_write_tokens);
	save(s);
}

void RecordTool(const std::string& tool_name, int result_bytes,
				 long savedBytes) {
	json s = load();
	auto& tc = s["tool_calls"];
	if (!tc.is_object()) tc = json::object();
	if (!tc.contains(tool_name)) {
		tc[tool_name] = {{"count", 0LL}, {"bytes", 0LL}, {"saved_bytes", 0LL}};
	}
	tc[tool_name]["count"] = tc[tool_name].value("count", 0LL) + 1LL;
	tc[tool_name]["bytes"] = tc[tool_name].value("bytes", 0LL) + static_cast<long long>(result_bytes);
	if (savedBytes > 0) {
		tc[tool_name]["saved_bytes"] =
			// Read the new key first; fall back to the legacy "fSavedbytes"
			// key so existing stats.json files are not silently zeroed out.
			tc[tool_name].value("saved_bytes",
				tc[tool_name].value("fSavedbytes", 0LL))
			+ static_cast<long long>(savedBytes);
	}
	save(s);
}

std::string FormatDisplay() {
	const json s = load();
	const std::string since = s.value("first_session", std::string{"(unknown)"});
	const int  sessions = s.value("sessions", 0LL);
	const int  turns    = s.value("turns", 0LL);
	const long long in_tok     = s.value("input_tokens",       0LL);
	const long long out_tok    = s.value("output_tokens",      0LL);
	const long long c_read_tok = s.value("cache_read_tokens",  0LL);
	const long long c_write_tok= s.value("cache_write_tokens", 0LL);

	// Lifetime cost using Sonnet rates:
	//   $3.00 / M input (uncached)
	//   $15.00 / M output
	//   $0.30 / M cache reads  (10% of input rate)
	//   $3.75 / M cache writes (125% of input rate)
	const double est_cost = (in_tok      / 1'000'000.0) * 3.0
						  + (out_tok     / 1'000'000.0) * 15.0
						  + (c_read_tok  / 1'000'000.0) * 0.30
						  + (c_write_tok / 1'000'000.0) * 3.75;

	// BFS counters — computed first so the summary block can
	// lead with the savings number.
	long long read_attr_saved = 0, read_attr_used = 0;
	int  read_attr_calls = 0;
	long long query_saved     = 0, query_used    = 0;
	int  query_calls     = 0;
	if (s.contains("tool_calls") && s["tool_calls"].is_object()) {
		const auto& tc = s["tool_calls"];
		if (tc.contains("ReadAttr")) {
			// Read the new key; fall back to the legacy "fSavedbytes" spelling.
			read_attr_saved = tc["ReadAttr"].value("saved_bytes",
				tc["ReadAttr"].value("fSavedbytes", 0LL));
			read_attr_used  = tc["ReadAttr"].value("bytes",       0LL);
			read_attr_calls = tc["ReadAttr"].value("count",       0LL);
		}
		if (tc.contains("Query")) {
			query_saved = tc["Query"].value("saved_bytes",
				tc["Query"].value("fSavedbytes", 0LL));
			query_used  = tc["Query"].value("bytes",       0LL);
			query_calls = tc["Query"].value("count",       0LL);
		}
	}
	const long long totalSavedBytes = read_attr_saved + query_saved;
	const long long totalUsedBytes  = read_attr_used  + query_used;
	const long long savedTokens     = totalSavedBytes / 4;
	const long long usedTokens      = totalUsedBytes  / 4;
	const long long fullTokens      = savedTokens + usedTokens;
	const int  bfsPct           = fullTokens > 0
		? static_cast<int>((savedTokens * 100) / fullTokens)
		: 0;
	const double bfsCostSaved  = (savedTokens / 1'000'000.0) * 3.0;

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
		"  Cache R:   %s tokens  (prompt cache hits)\n"
		"  Cache W:   %s tokens  (prompt cache writes)\n"
		"  Output:    %s tokens\n"
		"  Est cost:  $%.2f  (Sonnet rates)\n",
		sessions, turns,
		thousands(in_tok).c_str(),
		thousands(c_read_tok).c_str(),
		thousands(c_write_tok).c_str(),
		thousands(out_tok).c_str(),
		est_cost);
	out += buf;

	// ── BFS showcase block ───────────────────────────────
	out += "\n";
	out += "  \xE2\x94\x83 BFS — the Haiku advantage\n";
	out += "  \xE2\x94\x83\n";
	if (savedTokens > 0) {
		std::snprintf(buf, sizeof(buf),
			"  \xE2\x94\x83  Saved %s tokens  (%d%% of full-read cost)\n"
			"  \xE2\x94\x83  Cost avoided: $%.4f\n"
			"  \xE2\x94\x83\n"
			"  \xE2\x94\x83  %d BFS calls  (%d ReadAttr + %d Query)\n"
			"  \xE2\x94\x83  Tokens they used:     %8s\n"
			"  \xE2\x94\x83  Tokens they avoided:  %8s\n",
			thousands(savedTokens).c_str(), bfsPct, bfsCostSaved,
			read_attr_calls + query_calls,
			read_attr_calls, query_calls,
			thousands(usedTokens).c_str(),
			thousands(savedTokens).c_str());
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
