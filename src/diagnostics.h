#ifndef HAIKU_CLAUDE_CLI_DIAGNOSTICS_H
#define HAIKU_CLAUDE_CLI_DIAGNOSTICS_H

#include <string>

// diagnostics — assemble a human-readable status report shown by the
// CLI's `/doctor` command and the GUI's Help ▸ Diagnostics panel.
// Surfaces runtime context that is otherwise invisible: the active model,
// which CLAUDE.md memory files are loaded, the installed Agent Skills
// (with usage, lifecycle state, and — importantly — whether each one is
// actually advertised to Claude), the configured MCP servers (with tool
// counts / liveness), and the registered lifecycle hooks.
//
// The report is a plain-text string; callers just print it or drop it
// into a read-only BTextView. Kept out of chat_window so the assembly
// logic is small, shared, and self-contained.

namespace diagnostics {

// Build the full report. `model` and `workingDir` come from the caller
// (they live on the window); everything else is queried from the mcp::,
// hooks::, and paths:: subsystems. `version` is stamped into the header.
std::string BuildReport(const std::string& model,
                        const std::string& workingDir,
                        const std::string& version);

} // namespace diagnostics

#endif // HAIKU_CLAUDE_CLI_DIAGNOSTICS_H
