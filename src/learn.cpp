#include "learn.h"

#include "paths.h"

namespace learn {

namespace {

// House rules for skill authoring, embedded in the prompt so the model
// produces a SKILL.md that skills.cpp can parse and that stays cheap to
// carry in the system prompt.
//
// The description limit is the load-bearing rule: skills::SystemBlock()
// puts every model-invocable skill's description into the system prompt,
// which is now the cached stable tier. A rambling description inflates
// that prefix on every single request of every session, so the prompt
// tells the model to count characters rather than trusting its estimate.
const char* const kAuthoringStandards =
	"Follow these skill-authoring standards exactly.\n"
	"\n"
	"Frontmatter (YAML between --- markers at the top of SKILL.md):\n"
	"- name: lowercase-hyphenated, no spaces. This is the invocation\n"
	"  name — the user runs it as /<name>.\n"
	"- description: ONE sentence, 60 characters or fewer, ending with a\n"
	"  period. State the capability, not the implementation. No marketing\n"
	"  words (powerful, comprehensive, seamless, advanced, robust). Do not\n"
	"  repeat the skill name inside it. If it contains a colon, wrap the\n"
	"  whole value in double quotes.\n"
	"  This limit is NOT cosmetic: every model-invocable skill's\n"
	"  description is loaded into the system prompt of every session, so\n"
	"  each extra character is paid for on every request. After writing\n"
	"  the description, COUNT the characters; if it is over 60, cut it\n"
	"  down before saving — do not ship a long sentence and hope.\n"
	"    Good (45): `Search arXiv papers by keyword or author.`\n"
	"    Bad (118): `A comprehensive skill that lets the agent search\n"
	"                arXiv for academic papers using keywords, authors,\n"
	"                and categories.`\n"
	"- allowed-tools: optional, space-separated tool allow list.\n"
	"- disable-model-invocation: set true ONLY if the skill should be\n"
	"  user-invoked (/<name>) and never triggered autonomously.\n"
	"\n"
	"Body section order (omit a section only if it has no real content):\n"
	"1. `# <Human Title>` then 2-3 sentences: what it does, what it does\n"
	"   NOT do, and the key dependency stance (e.g. \"no external deps\").\n"
	"2. `## When to Use` — bullet list of concrete trigger phrases.\n"
	"3. `## Prerequisites` — exact packages, env vars, credentials.\n"
	"4. `## How to Run` — the canonical invocation.\n"
	"5. `## Quick Reference` — a flat command list, no narration.\n"
	"6. `## Procedure` — numbered steps with copy-paste-exact commands.\n"
	"7. `## Pitfalls` — known limits and things that look broken but aren't.\n"
	"8. `## Verification` — one command/check that proves it worked.\n"
	"\n"
	"Tool framing (this is what makes it a skill, not a shell cheatsheet):\n"
	"- Reference the tools by name in backticks: `Read`, `Write`, `Edit`,\n"
	"  `Bash`, `Grep`, `Glob`, `WebFetch`, `Task`, `ReadAttr`, `WriteAttr`,\n"
	"  `Query`, `TodoWrite`.\n"
	"- Do NOT name shell utilities that are already wrapped: say `Read`\n"
	"  not cat/head/tail, `Grep` not grep/rg, `Glob` not find/ls, `Write`\n"
	"  not echo>file or heredocs, `Edit` not sed/awk.\n"
	"- Third-party CLIs (ffmpeg, git, a compiler) are fine, but frame them\n"
	"  as \"invoke through the `Bash` tool\".\n"
	"- On Haiku, prefer the BFS attribute tools where they apply:\n"
	"  `ReadAttr`/`WriteAttr` for metadata, `Query` for indexed lookups.\n"
	"\n"
	"Skill body expansions available at invocation time:\n"
	"- `{{args}}` is replaced by whatever text follows /<name>.\n"
	"- ``!`shell command` `` is replaced by that command's stdout, so the\n"
	"  instructions arrive with live data already inlined. Use this for\n"
	"  context the skill always needs (e.g. ``!`git status --short` ``).\n"
	"\n"
	"Quality bar:\n"
	"- Prefer exact commands, paths, and signatures that appear VERBATIM\n"
	"  in the source. NEVER invent flags, paths, or APIs — if you did not\n"
	"  see it in the source, do not write it.\n"
	"- Keep it tight and scannable: ~100 lines for a simple skill, ~200\n"
	"  for a complex one. Do not re-paste the source documentation.\n"
	"- Do not write a router/index skill that only points at other skills.\n"
	"- Larger scripts belong in a `scripts/` file inside the skill\n"
	"  directory, referenced from SKILL.md by relative path — not inlined\n"
	"  for the model to re-type on every run.";

} // namespace

std::string BuildPrompt(const std::string& request) {
	std::string req = request;
	// Trim surrounding whitespace.
	const auto first = req.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		req.clear();
	} else {
		const auto last = req.find_last_not_of(" \t\r\n");
		req = req.substr(first, last - first + 1);
	}

	if (req.empty()) {
		req = "the workflow we just went through in this conversation — "
			  "review the steps taken and distill them into a reusable skill";
	}

	std::string out;
	out += "[/learn] The user wants you to learn a reusable skill from the "
		   "request below, and save it.\n\n";
	out += "THE REQUEST:\n" + req + "\n\n";
	out += "The request is open-ended and may mix two kinds of content, in "
		   "any order: SOURCES to gather (directories, file paths, URLs, "
		   "\"what we just did\", pasted notes) AND REQUIREMENTS that shape "
		   "the skill (what to focus on, what to leave out, scope, naming, "
		   "the angle to take). Treat EVERY part of the request as "
		   "load-bearing. In particular, prose that comes after a path or "
		   "link is NOT incidental — it is the user telling you what they "
		   "want from that source. A request like `<url> focus on the auth "
		   "flow, skip the deprecated endpoints` means: gather the URL AND "
		   "honor \"focus on auth, skip deprecated\" as authoring "
		   "requirements. Never fetch the first source and ignore the rest.\n\n";
	out += "Do this:\n";
	out += "1. Gather every source the user named, using the tools you "
		   "already have — `Read`/`Grep`/`Glob` for local files and "
		   "directories, `WebFetch` for URLs, the current conversation "
		   "history if they referred to something you just did, and the text "
		   "they pasted as-is. If the request is ambiguous about scope, make "
		   "a reasonable choice and note it; do not stall.\n";
	out += "2. Apply every requirement, focus, and constraint in the request "
		   "to the skill you author — these govern what the SKILL.md covers "
		   "and emphasizes, not just which sources you read.\n";
	out += "3. Author ONE SKILL.md and save it with the `Write` tool to\n";
	out += "   " + paths::UserSkillsDir() + "/<skill-name>/SKILL.md\n";
	out += "   (use " + paths::ProjectSkillsDir() + "/<skill-name>/SKILL.md "
		   "instead if the skill is specific to this project and should be "
		   "checked into version control). If the procedure needs a "
		   "non-trivial script, add it under that skill directory's "
		   "`scripts/` and reference it by relative path.\n\n";
	out += kAuthoringStandards;
	out += "\n\nWhen done, tell the user the skill name, where it was saved, "
		   "and a one-line summary of what it captured. Mention that it is "
		   "available as /<skill-name> in the next session.";
	return out;
}

} // namespace learn
