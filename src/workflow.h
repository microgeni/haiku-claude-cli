#ifndef HAIKU_CLAUDE_CLI_WORKFLOW_H
#define HAIKU_CLAUDE_CLI_WORKFLOW_H

#include <string>

#include <nlohmann/json.hpp>

// workflow — the bridge between the CLI's tool loop and the Markov
// "workflow memory" (src/markov.h). It is fed from the PostToolUse hook
// point in api.cpp: after every tool runs, Observe() turns the tool call
// into a canonical event key, learns it, and — when the event is unusually
// surprising given recent history — returns a short human-readable nudge.
//
// This is the lightweight, dependency-free alternative to a full HTM cortex
// (see the concept doc). It learns *the shape of your workflow* on this repo
// and flags anomalies; it does not generate code.
//
// Design notes:
//   * State is per-repo, keyed by working directory, persisted under
//     paths::WorkflowDir() so it accumulates across sessions.
//   * Off by default. Enable with "workflow": { "enabled": true } in
//     config.json (see Configure()).
//   * The whole thing is best-effort: any failure is swallowed so the tool
//     loop is never disrupted by workflow bookkeeping.
//   * Wired at both entry points — main.cpp (CLI) and app_main_gui.cpp (GUI);
//     the Observe() calls fire from the shared SendConversation engine in
//     api.cpp, so both front-ends learn identically once Begin() has bound a
//     repo.
//   * LIMITATION (GUI): the model is currently process-global. The GUI can
//     open several session windows on different repos (File ▸ New Session,
//     the 'ASKP' IPC); today they all share one model bound to whichever repo
//     was bound last. A follow-up should key models by session/window so
//     each window learns its own repo. The CLI (one process, one repo) is
//     unaffected.
namespace workflow {

using json = nlohmann::json;

// Apply configuration from the "workflow" object in config.json. Accepts:
//   enabled            (bool)   default false
//   surprise_threshold (double) 0..1, default 0.85
//   order              (int)    n-gram context length, default 2
//   nudges             (bool)   whether Observe() may return a nudge string;
//                               default true (set false to learn silently)
// Unknown/missing keys keep their defaults. Safe to call more than once.
void Configure(const json& workflow_config);

// True once Configure() has enabled the feature.
bool Enabled();

// Bind the model to a repository (its working directory). Loads any
// previously-saved state for this repo. Call once per session before the
// first Observe(); calling again with a new dir switches repos (saving the
// previous one first). A no-op when the feature is disabled.
void Begin(const std::string& working_dir);

// Feed one completed tool call. Builds a canonical event key from the tool
// name, its input, and whether it errored; learns it; and returns a nudge
// string when the event is anomalous *and* nudges are enabled — otherwise
// an empty string. Never throws; a disabled feature returns "".
//
//   tool_name  e.g. "Bash", "Edit", "Read"
//   tool_input the tool's input JSON (paths, commands, …)
//   is_error   did the tool report failure?
std::string Observe(const std::string& tool_name,
                    const json& tool_input,
                    bool is_error);

// Persist the current repo's learned state now. Called automatically at
// session end, but exposed for explicit flushing. No-op when disabled.
void Flush();

// Build the canonical event key for a tool call. Exposed for unit testing;
// the exact string format is an internal detail, not a stable contract.
//   "edit:src/auth.py"  /  "run:bash:ok"  /  "read:src/markov.h"
std::string MakeKey(const std::string& tool_name,
                    const json& tool_input,
                    bool is_error);

} // namespace workflow

#endif
