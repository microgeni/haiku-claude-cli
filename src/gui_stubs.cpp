// gui_stubs.cpp — minimal stub implementations for terminal-specific
// symbols referenced by api.cpp and tui.cpp that the GUI build
// doesn't use at runtime.

#include "terminal_sink.h"
#include "repl.h"
#include "tui.h"

// ── repl:: stubs ─────────────────────────────────────────────────────────────
namespace repl {
int  RealTtyFd()     { return -1; }
void WakeReadMessage() {}
void BlockStdin()    {}
void UnblockStdin()  {}
void RequestClearEditBuffer() {}
} // namespace repl

// ── TerminalSink stubs ───────────────────────────────────────────────────────
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
