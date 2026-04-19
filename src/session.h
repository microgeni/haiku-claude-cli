#ifndef HAIKU_CLAUDE_CLI_SESSION_H
#define HAIKU_CLAUDE_CLI_SESSION_H

#include <string>
#include <vector>

#include "config.h"

// Interactive REPL session: prompt loop, libedit integration,
// drag-and-drop attachment handling, remote-control wiring, and
// per-turn bookkeeping. Called from main() when -i / --interactive
// is set (or when stdin is a TTY and no one-shot message was given).
namespace session {

// Compose the preamble block announcing attached files to Claude.
// Returns an empty string when no paths are attached. Shared with
// the one-shot path so drops and --attach produce identical shapes.
std::string ComposeAttachmentPreamble(const std::vector<std::string>& paths);

// Dim `[attached: a, b, +N more]` one-liner shown after a successful
// --attach or drop. Keeps the list short on narrow terminals.
std::string FormatAttachedLine(const std::vector<std::string>& paths);

// Run the REPL. Returns the intended process exit code.
int InteractiveLoop(const config::Auth& initial_auth, const config::Config& cfg,
                    const std::string& initial_model, int max_tokens,
                    const std::string& custom_system,
                    const nlohmann::json& prices, bool resume,
                    const std::string& resume_name,
                    const std::string& initial_message,
                    std::vector<std::string> initial_attachments);

} // namespace session

#endif
