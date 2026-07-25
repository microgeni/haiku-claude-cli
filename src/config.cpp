#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "oauth.h"
#include "paths.h"
#include "skills.h"
#include "agents.h"
#include "api.h"       // api::g_plan_mode (plan-mode directive)
#include "history_util.h"

namespace config {

// The marketing version is injected at build time from the top-level VERSION
// file via -DCCH_VERSION (see Makefile), so it always matches the shipped
// package. The fallback only applies to ad-hoc builds without the define.
#ifndef CCH_VERSION
#define CCH_VERSION "0.0.0-dev"
#endif
const char* const kVersion      = CCH_VERSION;
const char* const kDefaultModel = "claude-sonnet-4-6";
const char* const kApiVersion   = "2023-06-01";
const char* const kOAuthBeta    = "oauth-2025-04-20";
const int         kMaxTokens    = 8192;

namespace {

std::ofstream g_log;

// Snapshot of claude:summary attributes across the project cwd.
// Taken once per session (first ComposeSystem call) so later turns
// use the cached result instead of re-walking the filesystem.
bool        g_bfs_loaded = false;
std::string g_bfs_snapshot;

std::string LoadOptionalFile(const std::string& path) {
	std::ifstream f(path);
	if (!f.is_open()) return {};
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Strict UTF-8 validation — returns true only if every byte is part
// of a well-formed sequence. catattr output from a raw-typed or
// binary-valued claude:summary attribute can contain 0xFF / 0xFE
// bytes that nlohmann::json refuses to serialize, so lines that fail
// this check get dropped from the preload snapshot.
bool IsValidUtf8(const char* data, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned char c = static_cast<unsigned char>(data[i]);
		int need = 0;
		if (c < 0x80) { ++i; continue; }
		else if ((c & 0xE0) == 0xC0) need = 1;
		else if ((c & 0xF0) == 0xE0) need = 2;
		else if ((c & 0xF8) == 0xF0) need = 3;
		else                         return false;
		if (i + need >= len) return false;
		for (int k = 1; k <= need; ++k) {
			unsigned char cc = static_cast<unsigned char>(data[i + k]);
			if ((cc & 0xC0) != 0x80) return false;
		}
		i += need + 1;
	}
	return true;
}

#ifdef __HAIKU__
void PreloadBfsSummaries() {
	if (g_bfs_loaded) return;
	g_bfs_loaded = true;

	// Walk the project cwd, collect claude:summary values via catattr.
	// `find | while read` is simple and handles non-ASCII paths
	// adequately for this use. Excludes the usual noise directories.
	// Redirect stdin from /dev/null so the sh child (and its subprocesses
	// catattr, find) never inherit the REPL's tty as fd 0 — a child
	// reading from the tty would consume keystrokes or trigger a spurious
	// EOF that causes readline() to return nullptr and exit the REPL.
	const char* cmd =
		"find . -type f "
		"  -not -path './.git/*' "
		"  -not -path './build/*' "
		"  -not -path './node_modules/*' "
		"  -not -path '*/\\.*' 2>/dev/null | while read f; do "
		"    s=$(catattr -d claude:summary \"$f\" 2>/dev/null) || continue; "
		"    [ -n \"$s\" ] && printf '%s :: %s\\n' \"$f\" \"$s\"; "
		"done < /dev/null";
	FILE* p = popen(cmd, "r");  // flawfinder: ignore
	if (!p) return;
	char buf[4096];
	while (std::fgets(buf, sizeof(buf), p)) {
		const size_t n = std::strlen(buf);
		if (!IsValidUtf8(buf, n)) continue;
		g_bfs_snapshot.append(buf, n);
	}
	pclose(p);
}
#else
void PreloadBfsSummaries() { g_bfs_loaded = true; }
#endif

// Maximum number of summary lines before per-turn refresh is skipped.
static constexpr int kSnapshotLineCap = 500;

// Count newline-terminated lines in s.
static int count_lines(const std::string& s) {
	int n = 0;
	for (char c : s) if (c == '\n') ++n;
	return n;
}

std::string BfsSystemBlock() {
#ifdef __HAIKU__
	if (!g_bfs_loaded) PreloadBfsSummaries();

	std::string s =
		"Haiku BFS attribute tools — prefer these on this project:\n"
		"- ReadAttr: read `claude:summary` (or any named attribute) from a "
		  "file. Check this BEFORE calling Read on a source file — summaries "
		  "cost ~10-30 tokens vs. thousands for full reads.\n"
		"- WriteAttr: write a one-line `claude:summary` after reading a file "
		  "for the first time. Later sessions will use ReadAttr and skip the "
		  "full read. Only use the `claude:*` namespace; never overwrite "
		  "BEOS:*/MAIL:*/Audio:* or other system attributes.\n"
		"- Query: BFS query expression for filesystem searches. Fast when an "
		  "index exists; use for file-metadata lookups.\n";

	if (!g_bfs_snapshot.empty()) {
		s += "\nFiles in this project with existing claude:summary "
			 "(prefer ReadAttr over Read for these):\n";
		s += g_bfs_snapshot;
		if (count_lines(g_bfs_snapshot) >= kSnapshotLineCap) {
			s += "(Note: project has 500+ summaries — mid-session WriteAttr "
				 "updates to claude:summary are not reflected until the next "
				 "session or /compact. Use ReadAttr to get the current value "
				 "for any specific file.)\n";
		}
	} else {
		s += "\n(No claude:summary attributes seeded yet — writing summaries "
			 "for source files you read this session will let later sessions "
			 "save tokens via ReadAttr.)\n";
	}
	return s;
#else
	return {};
#endif
}

// Behaviour guidelines injected into every session to improve the
// interactive experience. Covers three UX improvements observed from
// studying Claude Code's interaction patterns:
//
//   #3 — Intent narration: briefly state what you are about to do
//        before invoking a tool, so the user understands the plan
//        without waiting for the tool result.
//
//   #4 — Proactive next-step suggestions: after completing a task
//        (especially code), volunteer the obvious follow-up (compile,
//        run, test) as a short note — do not wait to be asked.
//
//   #5 — Post-tool summary: after a tool result arrives, emit one
//        plain-English sentence summarising what was done or found,
//        rather than (or in addition to) quoting the raw output.
std::string BehaviorSystemBlock() {
	return
		"## Interaction style\n"
		"\n"
		"Before calling a tool, write one short sentence describing what "
		"you are about to do and why. For example: \"I'll read the file to "
		"check the current implementation.\" or \"Creating the file now.\"\n"
		"\n"
		"After a tool returns, summarise the outcome in one plain-English "
		"sentence rather than repeating the raw output verbatim. For example: "
		"\"The file has 142 lines and imports curl and nlohmann/json.\" or "
		"\"Wrote hello_world.cpp successfully.\"\n"
		"\n"
		"After completing a task that involves code (creating, editing, or "
		"building a file), proactively suggest the next logical step — such "
		"as how to compile or run it — as a brief closing note, unless the "
		"user has already indicated what they want to do next.";
}

#ifdef __HAIKU__
// "What is this file about" distilled from content Claude just Read.
// No API call — first non-blank, non-comment line plus total-line-
// count, truncated to 80 chars.
std::string DeriveHeuristicSummary(const std::string& content) {
	size_t pos = 0;
	int    total_lines = 0;
	std::string first_meaningful;
	while (pos < content.size()) {
		const size_t nl  = content.find('\n', pos);
		const size_t len = (nl == std::string::npos ? content.size() : nl) - pos;
		++total_lines;
		if (first_meaningful.empty() && total_lines <= 50) {
			const std::string line = content.substr(pos, len);
			const size_t ws = line.find_first_not_of(" \t");
			if (ws != std::string::npos) {
				const std::string trimmed = line.substr(ws);
				if (trimmed.size() >= 2 &&
					trimmed.substr(0, 2) != "//" &&
					trimmed.substr(0, 2) != "/*" &&
					trimmed[0] != '#' && trimmed[0] != '*') {
					first_meaningful = trimmed.substr(0, 80);
				}
			}
		}
		if (nl == std::string::npos) break;
		pos = nl + 1;
	}
	std::string out = std::to_string(total_lines) + "L";
	if (!first_meaningful.empty()) out += " \xE2\x80\x94 " + first_meaningful;
	return out;
}

bool HasClaudeSummary(const std::string& path) {
	const std::string cmd =
		"catattr -d claude:summary " + ShellSingleQuote(path) + " 2>/dev/null < /dev/null";
	FILE* p = popen(cmd.c_str(), "r");  // flawfinder: ignore
	if (!p) return false;
	char buf[256];
	size_t total = 0;
	while (std::fgets(buf, sizeof(buf), p)) total += std::strlen(buf);
	pclose(p);
	return total > 1;  // >1 accounts for a trailing newline
}
#endif

// Maximum tool-result content stored in history (bytes). The full
// output was already streamed to the terminal; storing only the head
// keeps history.json small while preserving enough for Claude to
// follow the conversation on --resume.
constexpr size_t kHistoryToolResultCap = 4096;

// Maximum number of messages kept when saving/loading a session.
// 200 messages ≈ 100 turns — enough context for any real session.
// Mutable so SetHistoryMessageCap() can override it from config.json;
// starts at the historical default.
size_t g_history_message_cap = 200;

// Trim tool_result content blocks inside a messages array so each
// individual result is at most kHistoryToolResultCap bytes. Returns
// a new array; the in-memory messages vector is not mutated so the
// live session context stays intact (only the saved file is capped).
json TrimToolResults(const json& messages) {
	json out = json::array();
	for (const auto& msg : messages) {
		if (msg.value("role", "") == "user" && msg["content"].is_array()) {
			json trimmed_content = json::array();
			for (const auto& block : msg["content"]) {
				if (block.value("type", "") == "tool_result") {
					std::string content = block.value("content", "");
					if (content.size() > kHistoryToolResultCap) {
						content.resize(kHistoryToolResultCap);
						content += "\n[... truncated for history storage ...]";
					}
					json b = block;
					b["content"] = content;
					trimmed_content.push_back(std::move(b));
				} else {
					trimmed_content.push_back(block);
				}
			}
			json m = msg;
			m["content"] = trimmed_content;
			out.push_back(std::move(m));
		} else {
			out.push_back(msg);
		}
	}
	return out;
}

} // namespace

Config Load() {
	Config cfg;
	cfg.model      = kDefaultModel;
	cfg.max_tokens = kMaxTokens;

	std::ifstream f(paths::ConfigPath());
	if (!f.is_open()) return cfg;

	try {
		const json j = json::parse(f);
		if (j.contains("model"))      cfg.model      = j["model"].get<std::string>();
		if (j.contains("max_tokens")) cfg.max_tokens = j["max_tokens"].get<int>();
		if (j.contains("history_max_messages") && j["history_max_messages"].is_number_integer())
			cfg.history_max_messages = j["history_max_messages"].get<int>();
		if (j.contains("thinking_budget") && j["thinking_budget"].is_number_integer())
			cfg.thinking_budget = j["thinking_budget"].get<int>();
		if (j.contains("system"))     cfg.system     = j["system"].get<std::string>();
		if (j.contains("show_usage")) cfg.show_usage = j["show_usage"].get<bool>();
		if (j.contains("prices"))       cfg.prices      = j["prices"];
		if (j.contains("hooks"))        cfg.hooks       = j["hooks"];
		if (j.contains("mcp_servers"))  cfg.mcp_servers = j["mcp_servers"];
		if (j.contains("telegram"))     cfg.telegram    = j["telegram"];
		// Accept both the current key and the legacy lowercase-t spelling
		// so existing config.json files continue to work after the rename.
		if (j.contains("fAllowDestructiveTools"))
			cfg.fAllowDestructiveTools = j["fAllowDestructiveTools"].get<bool>();
		else if (j.contains("fAllowDestructivetools"))
			cfg.fAllowDestructiveTools = j["fAllowDestructivetools"].get<bool>();
		if (j.contains("logging") && j["logging"].is_object()) {
			cfg.logging_enabled = j["logging"].value("enabled", false);
		}
		if (j.contains("notify") && j["notify"].is_object()) {
			cfg.notify_enabled          = j["notify"].value("enabled", true);
			cfg.notify_min_duration_sec = j["notify"].value("min_duration_seconds", 60.0);
		}
		if (j.contains("compact") && j["compact"].is_object()) {
			cfg.compact_auto_threshold = j["compact"].value("auto_threshold", 0.8);
			cfg.compact_context_window = j["compact"].value("context_window", 0);
		}
	} catch (const json::exception& e) {
		std::cerr << "warning: failed to parse " << paths::ConfigPath() << ": " << e.what() << "\n";
	}
	return cfg;
}

Auth ResolveAuth() {
	if (auto stored = LoadTokens(); stored) {
		if (stored->IsExpired()) {
			if (auto refreshed = RefreshTokens(*stored); refreshed) {
				SaveTokens(*refreshed);
				return {AuthKind::OAuth, refreshed->access_token};
			}
			std::cerr << "warning: OAuth refresh failed, falling back to API key\n";
		} else {
			return {AuthKind::OAuth, stored->access_token};
		}
	}

	if (const char* k = std::getenv("ANTHROPIC_API_KEY"); k && *k) {  // flawfinder: ignore
		return {AuthKind::ApiKey, k};
	}
	if (std::string stored = LoadApiKey(); !stored.empty()) {
		return {AuthKind::ApiKey, stored};
	}
	return {};
}

// Path to the stored API key file (0600). Sibling of config.json.
static std::string ApiKeyPath() {
	return paths::ConfigDir() + "/api_key";
}

bool SaveApiKey(const std::string& key) {
	std::string k = key;
	// Trim whitespace/newlines so a pasted key with a trailing newline
	// doesn't break the Authorization header.
	while (!k.empty() && (k.back() == '\n' || k.back() == '\r'
			|| k.back() == ' ' || k.back() == '\t')) k.pop_back();
	size_t i = 0;
	while (i < k.size() && (k[i] == ' ' || k[i] == '\t')) ++i;
	k = k.substr(i);
	if (k.empty()) return false;

	paths::MkdirP(paths::ConfigDir());
	const std::string path = ApiKeyPath();
	// O_CREAT|0600 so the key file is never world-readable, even
	// transiently (mirrors the OAuth credentials write).
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	FILE* fp = ::fdopen(fd, "w");
	if (!fp) { ::close(fd); return false; }
	const std::string out = k + "\n";
	const bool ok = std::fwrite(out.data(), 1, out.size(), fp) == out.size();
	std::fclose(fp);
	return ok;
}

std::string LoadApiKey() {
	std::ifstream f(ApiKeyPath());
	if (!f.is_open()) return {};
	std::string k;
	std::getline(f, k);
	while (!k.empty() && (k.back() == '\n' || k.back() == '\r'
			|| k.back() == ' ' || k.back() == '\t')) k.pop_back();
	return k;
}

bool ClearApiKey() {
	return std::remove(ApiKeyPath().c_str()) == 0;
}

SystemTiers ComposeSystemTiers(const std::string& flag_system,
                                const std::string& working_dir) {
	SystemTiers tiers;
	auto append = [](std::string& out, const std::string& chunk) {
		if (chunk.empty()) return;
		if (!out.empty()) out += "\n\n";
		out += chunk;
	};

	// ── Stable tier ────────────────────────────────────────────────
	// Everything here is byte-identical across every turn of a session
	// and, for the same project, across sessions too. Ordering it first
	// makes it an exact-match prefix Anthropic's cache can reuse, so a
	// fresh session pays the (large) BFS snapshot + skills index cost
	// once rather than on every turn.
	//
	// The CLAUDE.md files are re-read each turn and belong here despite
	// being user-editable: they change rarely, and when they do the
	// cache simply misses once and re-warms on the following turn.
	append(tiers.stable, LoadOptionalFile(paths::UserMemoryPath()));
	append(tiers.stable, LoadOptionalFile(paths::ProjectMemoryPath()));
	append(tiers.stable, BfsSystemBlock());
	append(tiers.stable, BehaviorSystemBlock());
	append(tiers.stable, skills::SystemBlock());
	append(tiers.stable, agents::SystemBlock());

	// ── Volatile tier ──────────────────────────────────────────────
	// Session-scoped or invocation-scoped: the plan-mode directive, the
	// -s/--system flag text, and the working directory. Small, so a
	// cache miss on this tail is cheap.

	// Plan mode: ask the model to research and present a plan instead of
	// acting. The read-only tool filter (tools::Definitions) is the hard
	// guarantee; this directive shapes the behaviour.
	//
	// This lives in the VOLATILE tier deliberately: plan mode toggles
	// mid-session (/plan, /execute), and anything that can change between
	// turns must sit after the stable cache breakpoint or every toggle
	// would invalidate the whole cached prefix.
	if (api::g_plan_mode.load(std::memory_order_relaxed)) {
		append(tiers.volatileTier,
			"PLAN MODE IS ACTIVE. Do not modify anything. You have only "
			"read-only tools (no Bash, Write, Edit, or other state-changing "
			"tools). Investigate the request using the available read tools, "
			"then present a concise, numbered plan of the changes you would "
			"make — files to touch, commands to run, and the order — and stop. "
			"Do not begin implementing until the user leaves plan mode "
			"(they will run /execute).");
	}
	append(tiers.volatileTier, flag_system);

	// Append the working directory so Claude always knows where relative
	// paths resolve — useful when the user runs the CLI from a project root.
	// The GUI passes an explicit path; the CLI falls back to getcwd().
	if (!working_dir.empty()) {
		append(tiers.volatileTier, "Working directory: " + working_dir);
	} else {
		char cwdbuf[4096];
		if (getcwd(cwdbuf, sizeof(cwdbuf)))
			append(tiers.volatileTier, std::string("Working directory: ") + cwdbuf);
	}

	return tiers;
}

std::string ComposeSystem(const std::string& flag_system,
                           const std::string& working_dir) {
	// Single source of truth: the flat prompt is just the tiers joined.
	const SystemTiers tiers = ComposeSystemTiers(flag_system, working_dir);
	if (tiers.stable.empty()) return tiers.volatileTier;
	if (tiers.volatileTier.empty()) return tiers.stable;
	return tiers.stable + "\n\n" + tiers.volatileTier;
}

std::string StableSystemPrefix() {
	// flag_system / working_dir live entirely in the volatile tier, so
	// passing empties here yields the same stable text the full call
	// would produce. Cheap: the BFS snapshot is cached per session.
	return ComposeSystemTiers({}, {}).stable;
}

// Replace every invalid UTF-8 byte (or truncated sequence) with the
// Unicode replacement character U+FFFD (0xEF 0xBF 0xBD) so that
// nlohmann::json never sees a byte that would trigger type_error.316.
std::string SanitizeUtf8(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	const unsigned char* p   = reinterpret_cast<const unsigned char*>(s.data());
	const unsigned char* end = p + s.size();
	while (p < end) {
		unsigned char c = *p;
		int need = 0;
		if      (c < 0x80)             { out += static_cast<char>(c); ++p; continue; }
		else if ((c & 0xE0) == 0xC0)   need = 1;
		else if ((c & 0xF0) == 0xE0)   need = 2;
		else if ((c & 0xF8) == 0xF0)   need = 3;
		else { out += "\xEF\xBF\xBD"; ++p; continue; }   // bad lead byte

		if (p + need >= end) {                             // truncated sequence
			out += "\xEF\xBF\xBD"; p = end; continue;
		}
		bool ok = true;
		for (int k = 1; k <= need; ++k) {
			if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
		}
		if (!ok) { out += "\xEF\xBF\xBD"; ++p; continue; }

		for (int k = 0; k <= need; ++k) out += static_cast<char>(p[k]);
		p += need + 1;
	}
	return out;
}

// CapHistoryMessages moved to history_util.{h,cpp} (pure JSON logic,
// unit-tested). Declared via history_util.h, included below.

std::optional<json> LoadHistory(const std::string& name) {
	const std::string path = name.empty()
		? paths::HistoryPath()
		: paths::NamedHistoryPath(name);
	std::ifstream f(path);
	if (!f.is_open()) return std::nullopt;
	try {
		json j = json::parse(f);
		if (!j.contains("messages") || !j["messages"].is_array())
			return std::nullopt;
		json msgs = j["messages"];
		msgs = CapHistoryMessages(msgs, g_history_message_cap);
		return msgs;
	} catch (...) {
	}
	return std::nullopt;
}

bool SaveHistory(const json& messages, const std::string& model,
                 const std::string& name) {
	const std::string path = name.empty()
		? paths::HistoryPath()
		: paths::NamedHistoryPath(name);
	const auto slash = path.rfind('/');
	if (slash == std::string::npos) return false;
	if (!paths::MkdirP(path.substr(0, slash))) return false;

	const json j = {
		{"messages", CapHistoryMessages(TrimToolResults(messages), g_history_message_cap)},
		{"model",    model},
		{"saved_at", static_cast<long>(std::time(nullptr))},
	};

	// Atomic write: serialize to a tmp file alongside the real path,
	// then rename(2) into place. A crash or power loss mid-write
	// leaves the previous good file intact.
	//
	// Open with O_CREAT|0600 so the file is never world-readable even
	// transiently — avoids the chmod-after-open TOCTOU race (CWE-362).
	const std::string tmp_path = path + ".tmp";
	{
		const int fd = ::open(tmp_path.c_str(),
		                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) return false;
		FILE* fp = ::fdopen(fd, "w");
		if (!fp) { ::close(fd); return false; }
		const std::string serialized = j.dump(2, ' ', false,
			json::error_handler_t::replace) + "\n";
		const bool ok = std::fwrite(serialized.data(), 1,
		                            serialized.size(), fp) == serialized.size();
		std::fclose(fp); // also closes fd
		if (!ok) {
			std::remove(tmp_path.c_str());
			return false;
		}
	}
	if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
		std::remove(tmp_path.c_str());
		return false;
	}
	return true;
}

void SetHistoryMessageCap(int cap) {
	// Ignore non-positive values so a mis-set config key can't silently
	// truncate every save to nothing.
	if (cap >= 1)
		g_history_message_cap = static_cast<size_t>(cap);
}

void InitLogging(bool enabled) {
	if (!enabled) return;
	const std::string dir = paths::LogDir();
	if (!paths::MkdirP(dir)) return;

	const std::time_t t = std::time(nullptr);
	std::tm            tm {};
	localtime_r(&t, &tm);
	char date[32];
	std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);

	g_log.open(dir + "/claude-" + date + ".log", std::ios::app);
}

void LogLine(const std::string& msg) {
	if (!g_log.is_open()) return;
	const std::time_t t = std::time(nullptr);
	std::tm            tm {};
	localtime_r(&t, &tm);
	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
	g_log << "[" << ts << "] " << msg << "\n";
	g_log.flush();
}

std::string ShellSingleQuote(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 2);
	out += '\'';
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else           out += c;
	}
	out += '\'';
	return out;
}

#ifdef __HAIKU__
void AutoWriteSummaryIfMissing(const std::string& path,
                                const std::string& content) {
	if (path.empty()) return;
	if (HasClaudeSummary(path)) return;
	const std::string summary = DeriveHeuristicSummary(content);
	if (summary.empty()) return;

	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0) {
		setsid();
		int devnull = ::open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		const char* argv[] = {
			"addattr", "-t", "string",
			"claude:summary", summary.c_str(), path.c_str(),
			nullptr
		};
		execvp("addattr", const_cast<char* const*>(argv));  // flawfinder: ignore
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
}
#else
void AutoWriteSummaryIfMissing(const std::string&, const std::string&) {}
#endif

#ifdef __HAIKU__
// Re-run the full filesystem walk and replace g_bfs_snapshot.
// Called after /compact so summaries written during the session are
// reflected in the next system-prompt composition.
void ReloadBfsSummaries() {
	g_bfs_loaded   = false;
	g_bfs_snapshot.clear();
	PreloadBfsSummaries();
}

// Update individual entries in g_bfs_snapshot for a specific set of
// paths whose claude:summary attribute may have been written or changed
// during this session.  Avoids a full filesystem walk — O(changed).
// Each entry in the snapshot has the form "<path> :: <summary>\n".
// For each changed path we read its current attribute and replace
// (or insert) the corresponding line.
void RefreshSummarySnapshot(const std::vector<std::string>& paths) {
	if (paths.empty()) return;

	// Skip per-turn refresh on large projects to avoid O(project)
	// work accumulating over many turns.
	if (count_lines(g_bfs_snapshot) >= kSnapshotLineCap) return;

	for (const auto& path : paths) {
		// Read the current claude:summary value for this path.
		std::string value;
		FILE* p = popen(  // flawfinder: ignore
			("catattr -d claude:summary " + ShellSingleQuote(path)
			 + " 2>/dev/null < /dev/null").c_str(), "r");
		if (p) {
			char buf[1024];
			while (std::fgets(buf, sizeof(buf), p)) {
				const size_t n = std::strlen(buf);
				if (n > 0 && buf[n-1] == '\n') {
					value.append(buf, n - 1);
				} else {
					value.append(buf, n);
				}
			}
			pclose(p);
		}

		// Build the canonical snapshot line prefix for this path.
		const std::string prefix = path + " :: ";

		// Remove any existing line for this path from the snapshot.
		std::string updated;
		updated.reserve(g_bfs_snapshot.size() + prefix.size() + value.size() + 1);
		size_t pos = 0;
		while (pos < g_bfs_snapshot.size()) {
			const size_t nl  = g_bfs_snapshot.find('\n', pos);
			const size_t end = (nl == std::string::npos)
				? g_bfs_snapshot.size()
				: nl + 1;
			const std::string line = g_bfs_snapshot.substr(pos, end - pos);
			if (line.rfind(prefix, 0) != 0) {
				updated.append(line);
			}
			pos = end;
		}

		// Insert the new entry (only when the attribute has a value
		// and contains valid UTF-8).
		if (!value.empty() && IsValidUtf8(value.c_str(), value.size())) {
			updated += prefix + value + "\n";
		}

		g_bfs_snapshot = std::move(updated);
	}

	// Ensure the snapshot is marked loaded so BfsSystemBlock()
	// doesn't trigger a fresh full walk on the next ComposeSystem call.
	g_bfs_loaded = true;
}
#else
void ReloadBfsSummaries() {}
void RefreshSummarySnapshot(const std::vector<std::string>&) {}
#endif

} // namespace config
