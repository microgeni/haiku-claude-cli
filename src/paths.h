#ifndef HAIKU_CLAUDE_CLI_PATHS_H
#define HAIKU_CLAUDE_CLI_PATHS_H

#include <string>

// Filesystem path helpers. Every on-disk artifact the CLI touches
// (config, history, REPL line history, logs, user-level CLAUDE.md)
// lives under a single per-user directory returned by config_dir().
//
// On Haiku we follow the platform convention and use
//   $HOME/config/settings/claude-cli
// On Linux/macOS we honor $XDG_CONFIG_HOME, falling back to
//   $HOME/.config/claude-cli
//
// The functions here are pure — they only read environment
// variables and build strings. mkdir_p() is the one exception:
// it creates the directory tree on disk (mode 0700), which is
// the single mutating primitive every other module needs.
namespace paths {

std::string config_dir();
std::string config_path();          // <config_dir>/config.json
std::string history_path();         // <config_dir>/history.json
std::string repl_history_path();    // <config_dir>/repl_history
std::string log_dir();              // <config_dir>/logs
std::string user_memory_path();     // <config_dir>/CLAUDE.md
std::string project_memory_path();  // ./CLAUDE.md (cwd-relative)

// Create `path` and any missing parent directories with mode 0700.
// Returns false only if a mkdir(2) call fails for a reason other
// than EEXIST. Existing directories are not an error.
bool mkdir_p(const std::string& path);

} // namespace paths

#endif
