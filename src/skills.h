#ifndef HAIKU_CLAUDE_CLI_SKILLS_H
#define HAIKU_CLAUDE_CLI_SKILLS_H

#include <string>
#include <vector>

// Agent Skills — Claude Code parity.
//
// A skill is a directory named <skill-name> containing a SKILL.md
// entry point. SKILL.md has two parts: optional YAML frontmatter
// between `---` markers, and a markdown body with the instructions
// Claude follows when the skill runs.
//
// Frontmatter fields (all optional):
//   name                      Display name; defaults to the directory.
//   description               What the skill does / when to use it.
//                             Surfaced to the model so it can invoke
//                             the skill automatically.
//   disable-model-invocation  When true the skill is user-invoke-only
//                             (typed as /skill-name); the model never
//                             triggers it. Defaults to false.
//   allowed-tools             Space- or comma-separated tool allow list
//                             (informational; advertised in the body).
//
// The body supports two expansions, applied when the skill is invoked:
//   {{args}}      replaced by the text following /skill-name.
//   !`shell cmd`  replaced by the command's stdout (dynamic context
//                 injection), so the instructions arrive with live
//                 data already inlined.
//
// Skills are loaded from the user dir (<ConfigDir>/skills) and the
// project dir (./.claude/skills). Project skills override user skills
// of the same name. A skill can be invoked directly with /skill-name,
// and — unless disable-model-invocation is set — the model can choose
// to invoke it via its description.
namespace skills {

struct Skill {
	std::string name;                 // invocation name (directory or `name`)
	std::string description;          // for model-invocation / listings
	std::string body;                 // raw markdown body (pre-expansion)
	std::string dir;                  // skill directory (for supporting files)
	std::string allowedTools;         // raw allowed-tools string, if any
	bool        disableModelInvocation = false;
};

// (Re)scan the user and project skill directories. Safe to call
// repeatedly; replaces the in-memory registry each time.
void Load(const std::string& userDir, const std::string& projectDir);

// All loaded skills, sorted by name.
const std::vector<Skill>& All();

// Sorted skill names (for tab completion / listings).
std::vector<std::string> Names();

// Look up a skill by name. Returns nullptr when absent.
const Skill* Find(const std::string& name);

// Expand a skill body into a ready-to-send user message: substitutes
// {{args}} and runs any !`cmd` dynamic-context lines, replacing each
// with the command's stdout. Returns the expanded text. `found` is set
// to false (and the empty string returned) when no such skill exists.
std::string Expand(const std::string& name, const std::string& args, bool& found);

// A block describing model-invocable skills for the system prompt, so
// the model knows what skills exist and when to invoke them. Returns an
// empty string when no model-invocable skills are loaded.
std::string SystemBlock();

} // namespace skills

#endif
