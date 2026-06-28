#include "paths.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>

namespace paths {

std::string ConfigDir() {
#ifdef __HAIKU__
	const char* home = std::getenv("HOME");  // flawfinder: ignore
	return std::string(home ? home : "/boot/home") + "/config/settings/claude-cli";
#else
	const char* xdg = std::getenv("XDG_CONFIG_HOME");  // flawfinder: ignore
	if (xdg && *xdg) return std::string(xdg) + "/claude-cli";
	const char* home = std::getenv("HOME");  // flawfinder: ignore
	return std::string(home ? home : ".") + "/.config/claude-cli";
#endif
}

std::string ConfigPath()         { return ConfigDir() + "/config.json"; }
std::string HistoryPath()        { return ConfigDir() + "/history.json"; }

// Sanitise `name` to only alphanumerics, hyphens, and underscores
// so it is safe to embed directly in a filename.
std::string NamedHistoryPath(const std::string& name) {
	std::string safe;
	safe.reserve(name.size());
	for (unsigned char c : name) {
		if (std::isalnum(c) || c == '-' || c == '_') safe += static_cast<char>(c);
		else safe += '_';
	}
	if (safe.empty()) safe = "default";
	return ConfigDir() + "/history-" + safe + ".json";
}

std::string ReplHistoryPath()   { return ConfigDir() + "/repl_history"; }
std::string LogDir()             { return ConfigDir() + "/logs"; }
std::string UserMemoryPath()    { return ConfigDir() + "/CLAUDE.md"; }
std::string ProjectMemoryPath() { return "CLAUDE.md"; }
std::string SessionsDir()       { return ConfigDir() + "/sessions"; }
std::string GuiPrefsPath()      { return ConfigDir() + "/gui_prefs.msg"; }

// Agent Skills (Claude Code parity). Personal skills live under the
// config dir; project skills live in ./.claude/skills, checked into
// the repo so a team shares them. Each skill is a directory named
// <skill-name> containing a SKILL.md entry point.
std::string UserSkillsDir()     { return ConfigDir() + "/skills"; }
std::string ProjectSkillsDir()  { return ".claude/skills"; }

// Subagents (Claude Code parity). Markdown files with YAML
// frontmatter; project definitions override user definitions.
std::string UserAgentsDir()     { return ConfigDir() + "/agents"; }
std::string ProjectAgentsDir()  { return ".claude/agents"; }

// Walk `path` one component at a time and mkdir each prefix. mkdir(2)
// returning EEXIST is fine — someone (maybe a previous call, maybe the
// user) already created it. Any other error aborts the walk.
bool MkdirP(const std::string& path) {
	std::string accum;
	for (size_t i = 0; i < path.size(); ++i) {
		accum += path[i];
		const bool boundary = (path[i] == '/') || (i + 1 == path.size());
		if (!boundary) continue;
		if (accum.empty() || accum == "/") continue;
		if (mkdir(accum.c_str(), 0700) != 0 && errno != EEXIST) return false;
	}
	return true;
}

bool EnsureParentDir(const std::string& filePath) {
	const auto slash = filePath.rfind('/');
	if (slash == std::string::npos) return true;
	return MkdirP(filePath.substr(0, slash));
}

} // namespace paths
