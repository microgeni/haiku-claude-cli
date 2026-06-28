#ifndef HAIKU_CLAUDE_CLI_PATHS_H
#define HAIKU_CLAUDE_CLI_PATHS_H

#include <string>

// Filesystem path helpers. Every on-disk artifact the CLI touches
// (config, history, REPL line history, logs, user-level CLAUDE.md)
// lives under a single per-user directory returned by ConfigDir().
//
// On Haiku we follow the platform convention and use
//   $HOME/config/settings/claude-cli
// On Linux/macOS we honor $XDG_CONFIG_HOME, falling back to
//   $HOME/.config/claude-cli
//
// The functions here are pure — they only read environment
// variables and build strings. MkdirP() is the one exception:
// it creates the directory tree on disk (mode 0700), which is
// the single mutating primitive every other module needs.
namespace paths {

std::string ConfigDir();
std::string ConfigPath();               // <ConfigDir>/config.json
std::string HistoryPath();              // <ConfigDir>/history.json
std::string NamedHistoryPath(         // <ConfigDir>/history-<name>.json
	const std::string& name);
std::string ReplHistoryPath();         // <ConfigDir>/repl_history
std::string LogDir();                  // <ConfigDir>/logs
std::string UserMemoryPath();          // <ConfigDir>/CLAUDE.md
std::string ProjectMemoryPath();       // ./CLAUDE.md (cwd-relative)
std::string SessionsDir();             // <ConfigDir>/sessions  (GUI)
std::string GuiPrefsPath();            // <ConfigDir>/gui_prefs.msg  (GUI)
std::string UserSkillsDir();           // <ConfigDir>/skills
std::string ProjectSkillsDir();        // ./.claude/skills (cwd-relative)
std::string UserAgentsDir();           // <ConfigDir>/agents
std::string ProjectAgentsDir();        // ./.claude/agents (cwd-relative)

// Create `path` and any missing parent directories with mode 0700.
// Returns false only if a mkdir(2) call fails for a reason other
// than EEXIST. Existing directories are not an error.
bool MkdirP(const std::string& path);

// Create all parent directories required for `filePath` to be
// written. Equivalent to `mkdir -p $(dirname filePath)`.
// Returns false only if a mkdir(2) call fails for a reason other
// than EEXIST. A no-op when the parent directory already exists.
bool EnsureParentDir(const std::string& filePath);

} // namespace paths

#endif
