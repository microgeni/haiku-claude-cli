// gui_stubs.cpp — minimal stub implementations for terminal-specific
// symbols referenced by api.cpp and tui.cpp that the GUI build
// doesn't use at runtime.
//
// The GUI always injects a GuiSink into SendWithTools, so the
// TerminalSink default-creation path in api.cpp is never reached.
// The tui:: functions that call repl:: are the terminal scroll-region
// and flush-timer helpers; they are no-ops in the GUI.

#include "terminal_sink.h"
#include "repl.h"
#include "stats.h"
#include "tui.h"

// ── repl:: stubs ─────────────────────────────────────────────────────────────
// The GUI has no libedit loop; these are never called.
namespace repl {
int  RealTtyFd()     { return -1; }
void WakeReadMessage() {}
void BlockStdin()    {}
void UnblockStdin()  {}
void RequestClearEditBuffer() {}
} // namespace repl

// ── stats:: stubs ────────────────────────────────────────────────────────────
namespace stats {
void RecordTool(const std::string&, int, long) {}
void RecordTurn(int, int, int, int) {}
void RecordSession() {}
} // namespace stats

// ── TerminalSink stubs ───────────────────────────────────────────────────────
// api.cpp creates a TerminalSink when no sink_in is provided; the GUI
// always passes a GuiSink, so these bodies are never executed.
TerminalSink::TerminalSink()  { /* no-op in GUI */ }
TerminalSink::~TerminalSink() { if (fSpinner) { delete fSpinner; fSpinner = nullptr; } }
void TerminalSink::SetLiveInputTokens(std::atomic<int>*) {}
void TerminalSink::OnText(const std::string&) {}
void TerminalSink::OnMeta(const std::string&) {}
void TerminalSink::OnDiag(const std::string&) {}
void TerminalSink::OnError(const std::string&) {}
void TerminalSink::OnToolStatus(const std::string&) {}
void TerminalSink::Flush() {}
api::Permission TerminalSink::AskPermission(const std::string&,
                                             const nlohmann::json&,
                                             std::string*) {
	return api::Permission::Deny;
}
