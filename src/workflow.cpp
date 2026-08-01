#include "workflow.h"

#include <algorithm>
#include <cctype>
#include <memory>

#include "markov.h"
#include "paths.h"

namespace workflow {

namespace {

// ---- feature state ---------------------------------------------------------

bool                                  g_enabled  = false;
bool                                  g_nudges   = true;
markov::Config                        g_cfg;                 // seeds new models
std::unique_ptr<markov::MarkovModel>  g_model;               // current repo
std::string                           g_state_path;          // where to persist
int                                   g_since_save = 0;      // dirty counter

// Persist every N observations so a crash loses at most N events, without
// writing on every single tool call.
constexpr int kSaveEvery = 10;

// Sanitize a working-dir path into a safe, stable filename stem.
std::string RepoStem(const std::string& dir) {
	std::string s;
	s.reserve(dir.size());
	for (unsigned char c : dir) {
		if (std::isalnum(c)) s += static_cast<char>(c);
		else if (c == '/' || c == '\\' || c == ' ') s += '_';
		// drop everything else (dots, colons, …)
	}
	// Trim leading underscores for tidiness.
	std::size_t start = s.find_first_not_of('_');
	if (start != std::string::npos) s = s.substr(start);
	if (s.empty()) s = "default";
	// Guard against pathologically long names.
	if (s.size() > 120) s = s.substr(s.size() - 120);
	return s;
}

// The last path-ish component and a couple of its parents, so related files
// in the same module share sub-tokens without the whole absolute path noise.
// "src/foo/auth.py" -> "foo/auth.py"
std::string TrimPath(std::string p) {
	if (p.empty()) return p;
	// Normalize separators to '/'.
	std::replace(p.begin(), p.end(), '\\', '/');
	// Keep at most the final two components.
	std::size_t last = p.find_last_of('/');
	if (last == std::string::npos) return p;
	std::size_t prev = (last == 0) ? std::string::npos : p.find_last_of('/', last - 1);
	return (prev == std::string::npos) ? p : p.substr(prev + 1);
}

// First whitespace-delimited token of a shell command — its program name.
// "git commit -m ..." -> "git"; "  ./run.sh" -> "./run.sh".
std::string FirstWord(const std::string& cmd) {
	std::size_t i = cmd.find_first_not_of(" \t\r\n");
	if (i == std::string::npos) return {};
	std::size_t j = cmd.find_first_of(" \t\r\n", i);
	return cmd.substr(i, (j == std::string::npos) ? std::string::npos : j - i);
}

std::string Lower(std::string s) {
	for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
	return s;
}

void SwitchRepo(const std::string& working_dir) {
	// Save whatever we had open first.
	Flush();

	g_model = std::make_unique<markov::MarkovModel>(g_cfg);
	const std::string stem = RepoStem(working_dir.empty() ? "." : working_dir);
	g_state_path = paths::WorkflowDir() + "/" + stem + ".json";
	g_model->Load(g_state_path);   // ok if missing — starts fresh
	g_model->ResetContext();       // new session = new rolling context
	g_since_save = 0;
}

} // namespace

// ---- public API ------------------------------------------------------------

void Configure(const json& wf) {
	if (!wf.is_object()) return;
	g_enabled = wf.value("enabled", g_enabled);
	g_nudges  = wf.value("nudges", g_nudges);
	g_cfg.surprise_threshold = wf.value("surprise_threshold", g_cfg.surprise_threshold);
	g_cfg.order              = wf.value("order", g_cfg.order);
	// New settings take effect on the next Begin() (which reseeds the model
	// from g_cfg). Configure() is normally called at startup, before Begin().
}

bool Enabled() { return g_enabled; }

void Begin(const std::string& working_dir) {
	if (!g_enabled) return;
	SwitchRepo(working_dir);
}

std::string MakeKey(const std::string& tool_name, const json& tinput, bool is_error) {
	const std::string t = Lower(tool_name);
	const std::string status = is_error ? "err" : "ok";

	// File-oriented tools: key on the (trimmed) path so edits to the same
	// module reinforce each other.
	if (t == "edit" || t == "write") {
		std::string p = TrimPath(tinput.value("path", std::string{}));
		return "edit:" + p + ":" + status;
	}
	if (t == "read") {
		std::string p = TrimPath(tinput.value("path", std::string{}));
		return "read:" + p;
	}
	if (t == "bash") {
		std::string prog = FirstWord(tinput.value("command", std::string{}));
		// Reduce a program path to its basename so "./x" and "/usr/bin/x"
		// don't fragment the vocabulary unnecessarily.
		std::replace(prog.begin(), prog.end(), '\\', '/');
		std::size_t slash = prog.find_last_of('/');
		if (slash != std::string::npos) prog = prog.substr(slash + 1);
		return "run:" + prog + ":" + status;
	}
	if (t == "grep" || t == "glob") {
		return "search:" + t;
	}
	if (t == "webfetch" || t == "websearch") {
		return "web:" + t;
	}
	if (t == "task") {
		return "task:" + status;
	}
	// Generic fallback: tool name + status. Keeps unknown/MCP tools coherent.
	return t + ":" + status;
}

std::string Observe(const std::string& tool_name, const json& tinput, bool is_error) {
	if (!g_enabled || !g_model) return {};

	std::string nudge;
	try {
		const std::string key = MakeKey(tool_name, tinput, is_error);

		// Capture what the model expected here *before* Observe() advances
		// the context. Afterwards Predict() answers "what usually follows
		// the anomaly?", which is not the question being asked — and for a
		// novel context it is almost always empty, so the "you usually do
		// X here" half of the nudge would never appear.
		const std::string expected = g_nudges ? g_model->Predict() : std::string{};

		g_model->Observe(key);

		if (g_nudges && g_model->LastWasAnomaly()) {
			// A phrased-later hook could hand the window to the LLM; for now a
			// compact, honest note. Keep it low-key — it's a hint, not an alarm.
			nudge = "\xE2\x9A\xA1 workflow: unusual step (" + key + ")";
			if (!expected.empty() && expected != key)
				nudge += " — you usually do '" + expected + "' here";
		}

		if (++g_since_save >= kSaveEvery) Flush();
	} catch (...) {
		// Never let workflow bookkeeping disrupt the tool loop.
		return {};
	}
	return nudge;
}

void Flush() {
	if (!g_model || g_state_path.empty()) return;
	// RepoStem() flattens the working directory into a single filename
	// component, so this resolves to WorkflowDir() itself. Going through
	// EnsureParentDir() keeps the call site correct if that naming ever
	// grows a subdirectory.
	paths::EnsureParentDir(g_state_path);
	g_model->Save(g_state_path);
	g_since_save = 0;
}

} // namespace workflow
