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

	// Usage telemetry, read from BFS attributes on SKILL.md (Haiku
	// only; zero/default elsewhere). See the Usage section below.
	int32_t     uses     = 0;         // claude:skill_uses
	time_t      lastUsed = 0;         // claude:skill_lastused (0 = never)
	std::string state;                // claude:skill_state ("" = active)
	bool        pinned   = false;     // claude:skill_pinned
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

// True when the named skill's body contains at least one !`cmd`
// dynamic-context marker — i.e. expanding it will execute shell.
//
// Typing /skill-name is explicit user consent to run that skill, but the
// model invoking it via the Skill tool is not, so the tool layer uses
// this to decide whether the call needs a permission prompt. Returns
// false for unknown skills.
bool BodyRunsShell(const std::string& name);

// Expand a skill body into a ready-to-send user message: substitutes
// {{args}} and runs any !`cmd` dynamic-context lines, replacing each
// with the command's stdout. Returns the expanded text. `found` is set
// to false (and the empty string returned) when no such skill exists.
//
// When `runShell` is false the !`cmd` markers are replaced with a
// placeholder instead of being executed. Plan mode uses this so the
// model can read a procedure without the act of reading it running
// commands — the read-only guarantee has to hold here too.
std::string Expand(const std::string& name, const std::string& args, bool& found,
                   bool runShell = true);

// A block for the system prompt describing model-invocable skills and
// how to create new ones.
//
// The index of existing skills is omitted when none are invocable, but
// the "creating skills" guidance is always present — a fresh install
// with no skills is precisely when the model needs to know the
// capability exists. Returns a non-empty string in all cases.
std::string SystemBlock();

// ── Usage telemetry and lifecycle ───────────────────────────────────
//
// Every invocation of a skill bumps two BFS attributes on its
// SKILL.md: a counter and a timestamp. Storing this in attributes
// rather than a sidecar JSON file means the data is queryable
// (`Query claude:skill_lastused < ...`), visible in Tracker, and
// survives the file being moved or copied — and it costs no parsing
// on load.
//
// Attributes written (all Haiku-only; no-ops elsewhere):
//   claude:skill_uses     int32   total invocations
//   claude:skill_lastused int64   unix seconds of the last invocation
//   claude:skill_state    string  "active" | "stale" | "archived"
//   claude:skill_pinned   bool    true = exempt from auto-transitions
//
// Lifecycle mirrors the states above: a skill unused for longer than
// the stale threshold becomes "stale"; one unused past the archive
// threshold becomes "archived". Nothing is ever auto-deleted, and
// pinned skills never transition. Archived skills stay on disk and
// stay invocable by name — they are only dropped from the system
// prompt, which is the thing that costs tokens on every request.

// Record one invocation of `name`: bump the counter, stamp the
// timestamp, and clear any stale/archived state (using a skill
// revives it). No-op when the skill doesn't exist or on non-Haiku.
void RecordUse(const std::string& name);

// Recompute lifecycle states from the last-used timestamps and write
// back any that changed. Skills that have never been used age from
// their SKILL.md modification time, so a freshly written skill is not
// immediately marked stale. Pinned skills are skipped. Returns the
// number of skills whose state changed.
int ApplyLifecycle(int staleAfterDays = 30, int archiveAfterDays = 90);

// Pin / unpin a skill (exempt it from automatic transitions).
// Returns false when no such skill exists.
bool SetPinned(const std::string& name, bool pinned);

// Ensure BFS indexes exist for the usage attributes so lifecycle
// queries are O(1). Safe to call on every launch.
void EnsureUsageIndexes();

} // namespace skills

#endif
