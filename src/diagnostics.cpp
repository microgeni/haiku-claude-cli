#include "diagnostics.h"

#include <ctime>
#include <fstream>

#include "hooks.h"
#include "mcp.h"
#include "paths.h"
#include "skills.h"

namespace diagnostics {

namespace {

// True if a regular file exists and can be opened for reading.
bool file_exists(const std::string& path) {
	std::ifstream f(path);
	return f.good();
}

// Why a skill is absent from the system prompt, or "" when it is present.
//
// A skill that Claude never picks up looks identical to a broken one from
// the outside, so the report has to name the actual reason. These mirror
// the skip conditions in skills::SystemBlock() — keep them in sync.
std::string prompt_exclusion_reason(const skills::Skill& s) {
	if (s.disableModelInvocation)
		return "disable-model-invocation is set";
	if (s.description.empty())
		return "no description in frontmatter";
	if (s.state == "archived")
		return "archived (unused >90d)";
	return {};
}

// "3d ago" / "today" / "never".
std::string last_used_phrase(const skills::Skill& s) {
	if (s.lastUsed == 0) return "never";
	const double days = std::difftime(std::time(nullptr), s.lastUsed) / 86400.0;
	if (days < 1.0) return "today";
	return std::to_string(static_cast<int>(days)) + "d ago";
}

} // namespace

std::string BuildReport(const std::string& model,
                        const std::string& workingDir,
                        const std::string& version) {
	std::string r;

	r += "Claude diagnostics\n";
	r += "==================\n\n";

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

	// ── Agent Skills ─────────────────────────────────────────────────────
	// The most useful thing this section answers is "why isn't Claude
	// using my skill?" — a skill can be on disk and invocable by name yet
	// absent from the system prompt (archived, no description, or
	// explicitly manual), which is otherwise completely invisible.
	r += "Agent Skills\n";
	r += "------------\n";
	{
		const auto& all = skills::All();
		if (all.empty()) {
			r += "  (none installed — add one under\n";
			r += "   " + paths::UserSkillsDir() + "/<name>/SKILL.md\n";
			r += "   or run /learn to distil one from this session)\n";
		} else {
			int hidden = 0;
			for (const auto& s : all) {
				const std::string why = prompt_exclusion_reason(s);
				if (!why.empty()) ++hidden;

				r += "  " + s.name;
				r += why.empty() ? "  [in prompt]" : "  [not in prompt]";
				if (s.pinned) r += " [pinned]";
				r += "\n";

				r += "      used " + std::to_string(s.uses) + "x, last "
				   + last_used_phrase(s) + "\n";
				if (skills::BodyRunsShell(s.name))
					r += "      runs shell on expand (asks permission)\n";
				if (!why.empty())
					r += "      hidden from Claude: " + why + "\n";
			}
			r += "\n  " + std::to_string(all.size() - static_cast<size_t>(hidden))
			   + " of " + std::to_string(all.size())
			   + " advertised to Claude in the system prompt.\n";
			if (hidden > 0) {
				r += "  Skills not in the prompt stay runnable by name\n";
				r += "  (/skill-name, or Tools > Skills in the GUI) but\n";
				r += "  Claude will not choose them on its own.\n";
			}
		}
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
