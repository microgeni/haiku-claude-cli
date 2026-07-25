#ifndef HAIKU_CLAUDE_CLI_LEARN_H
#define HAIKU_CLAUDE_CLI_LEARN_H

#include <string>

// `/learn` — turn whatever the user just described into a reusable
// Agent Skill.
//
// The command is deliberately open-ended: the user can point it at a
// directory of code, a documentation URL, a workflow they just walked
// through in this conversation, or pasted notes. Rather than building a
// separate distillation engine, `/learn` composes ONE instruction that
// the live model executes as a normal turn, using the tools it already
// has (Read, Grep, Glob, WebFetch, Write). That keeps the feature at
// zero new tool surface and means it works identically in the CLI, the
// GUI, and over the Telegram bridge.
//
// The prompt carries the skill-authoring standards inline so the model
// writes a SKILL.md matching the format skills.cpp parses — most
// importantly a short description, since skills::SystemBlock() is
// loaded into every system prompt and an overlong description wastes
// cached prefix on every session.
namespace learn {

// Build the instruction for an open-ended /learn request. `request` is
// the free text following the command; when empty, the prompt defaults
// to distilling the current conversation.
std::string BuildPrompt(const std::string& request);

} // namespace learn

#endif // HAIKU_CLAUDE_CLI_LEARN_H
