#include "diagnostics.h"

#include <fstream>

#include "hooks.h"
#include "mcp.h"
#include "paths.h"

namespace diagnostics {

namespace {

// True if a regular file exists and can be opened for reading.
bool file_exists(const std::string& path) {
	std::ifstream f(path);
	return f.good();
}

} // namespace

std::string BuildReport(const std::string& model,
                        const std::string& workingDir,
                        const std::string& version) {
	std::string r;

	r += "Claude GUI diagnostics\n";
	r += "======================\n\n";

	r += "Version:  " + (version.empty() ? std::string("(unknown)") : version) + "\n";
	r += "Model:    " + (model.empty() ? std::string("(default)") : model) + "\n";
	if (!workingDir.empty())
		r += "Work dir: " + workingDir + "\n";
	r += "\n";

	// ── Memory (CLAUDE.md) ───────────────────────────────────────────────
	r += "Memory (CLAUDE.md)\n";
	r += "------------------\n";
	{
		const std::string userPath = paths::UserMemoryPath();
		const std::string projPath = paths::ProjectMemoryPath();
		const bool userThere = file_exists(userPath);
		const bool projThere = file_exists(projPath);
		r += std::string("  user:    ") + (userThere ? "loaded  " : "none    ")
		   + userPath + "\n";
		r += std::string("  project: ") + (projThere ? "loaded  " : "none    ")
		   + projPath + "\n";
		if (!userThere && !projThere)
			r += "  (no memory files — create CLAUDE.md to add project context)\n";
	}
	r += "\n";

	// ── MCP servers ──────────────────────────────────────────────────────
	r += "MCP servers\n";
	r += "-----------\n";
	{
		const auto servers = mcp::ActiveServers();
		if (servers.empty()) {
			r += "  (none configured — add an \"mcp_servers\" key to config.json)\n";
		} else {
			for (const auto& s : servers) {
				r += "  " + s.name + "  ";
				r += s.alive ? "[alive]" : "[down]";
				r += "  " + std::to_string(s.tool_count) + " tool"
				   + (s.tool_count == 1 ? "" : "s") + "\n";
			}
		}
	}
	r += "\n";

	// ── Hooks ────────────────────────────────────────────────────────────
	r += "Hooks\n";
	r += "-----\n";
	{
		const auto summary = hooks::ActiveSummary();
		if (summary.empty()) {
			r += "  (none configured — add a \"hooks\" key to config.json)\n";
		} else {
			for (const auto& e : summary) {
				r += "  " + e.event + ": " + std::to_string(e.count)
				   + " hook" + (e.count == 1 ? "" : "s") + "\n";
			}
		}
	}

	return r;
}

} // namespace diagnostics
