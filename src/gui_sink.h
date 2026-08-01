#ifndef HAIKU_CLAUDE_CLI_GUI_SINK_H
#define HAIKU_CLAUDE_CLI_GUI_SINK_H

#include <atomic>
#include <semaphore.h>
#include <string>
#include <vector>

#include <Messenger.h>

#include "output_sink.h"
#include "structured_sink.h"

// BMessage 'what' codes that cross the worker→main-thread boundary.
// Defined here so both gui_sink.cpp and chat_window.cpp share them.
namespace gui {
constexpr uint32_t MSG_CHUNK       = 'CHNK'; // B_STRING_TYPE "text"
constexpr uint32_t MSG_THINKING    = 'THNK'; // B_STRING_TYPE "text" (dim reasoning)
constexpr uint32_t MSG_DONE        = 'DONE'; // turn complete
constexpr uint32_t MSG_TOOL_START  = 'TSTR'; // B_STRING_TYPE "name", "summary"
constexpr uint32_t MSG_TOOL_DONE   = 'TDNE'; // B_STRING_TYPE "name", B_BOOL_TYPE "ok"
constexpr uint32_t MSG_TOOL_DIFF   = 'TDIF'; // B_STRING_TYPE "diff" — prefixed diff block
constexpr uint32_t MSG_ASK_PERM    = 'APEM'; // B_STRING_TYPE "tool", "preview"
constexpr uint32_t MSG_ASK_CHOICE  = 'ACHO'; // "prompt" + B_STRING_TYPE[] "options"
constexpr uint32_t MSG_STATUS      = 'STAT'; // B_INT32_TYPE "kind" (sink::StatusKind)
constexpr uint32_t MSG_ERR         = 'RERR'; // B_STRING_TYPE "text"
constexpr uint32_t MSG_NOTICE      = 'NTCE'; // B_STRING_TYPE "text" — advisory line
constexpr uint32_t MSG_SEND        = 'SEND'; // input control / button → window
constexpr uint32_t MSG_WORKER_DONE = 'WDNE'; // worker thread finished
} // namespace gui

namespace gui {

// GuiSink — implements both OutputSink (so api::SendWithTools can use it
// unchanged) and sink::StructuredSink (the rich GUI model). Runs on the
// worker thread; all view mutations are marshalled to the main thread via
// BMessenger::SendMessage (non-blocking). AskPermission is the sole
// exception: it blocks the worker on fPermSem until ChatWindow shows a
// BAlert and calls DeliverPermissionReply().
//
// Lifetime: stack-allocated by ChatWindow::_LaunchWorker() for the
// duration of one api::SendWithTools call. Destroyed in MSG_WORKER_DONE
// after fWorker.join() confirms the thread has exited.
class GuiSink : public OutputSink, public sink::StructuredSink {
public:
	explicit GuiSink(BMessenger window);
	~GuiSink() override;

	GuiSink(const GuiSink&)            = delete;
	GuiSink& operator=(const GuiSink&) = delete;

	// ── sink::StructuredSink ─────────────────────────────────────────────
	void BeginMessage(const std::string& role) override;
	void AppendText(const std::string& chunk)  override;
	void EndMessage()                          override;
	void ToolStarted(const std::string& name,
	                 const std::string& summary) override;
	void ToolFinished(const std::string& name,
	                  bool ok,
	                  const std::string& detail) override;
	int  AskChoice(const std::string& prompt,
	               const std::vector<std::string>& options) override;
	sink::Permission AskPermission(const std::string& tool,
	                               const std::string& preview) override;
	void SetStatus(sink::StatusKind kind) override;
	void OnError(const std::string& message) override; // StructuredSink

	// ── OutputSink (adapter layer, removed when SendWithTools takes
	//    StructuredSink* directly in a future step) ─────────────────────
	void OnText(const std::string& chunk) override;
	void OnThinking(const std::string& chunk) override;
	void OnMeta(const std::string&)       override {} // suppressed in GUI
	void OnDiag(const std::string&)       override {} // suppressed in GUI
	// Not suppressed: an advisory the user should actually see. Rendered
	// as a tool-log line rather than assistant text.
	void OnNotice(const std::string& text) override;
	void OnToolStatus(const std::string& phase) override;
	api::Permission AskPermission(const std::string& tool_name,
	                              const nlohmann::json& input,
	                              std::string* denial_reason) override;

	// Called by ChatWindow::MessageReceived(MSG_ASK_PERM) on the main
	// thread after BAlert::Go() returns. Unblocks the worker thread.
	void DeliverPermissionReply(bool granted);

	// Called by ChatWindow on the main thread after the choice modal
	// closes. `index` is the 0-based chosen option, or -1 if cancelled.
	// Unblocks the worker thread parked in AskChoice().
	void DeliverChoiceReply(int index);

private:
	BMessenger         fWindow;
	sem_t              fPermSem;
	std::atomic<bool>  fPermResult { false };
	// Result delivered for an in-flight AskChoice (0-based index, -1 =
	// cancelled). Shares fPermSem because permission and choice prompts
	// never overlap — both block the single worker thread.
	std::atomic<int>   fChoiceResult { -1 };
	// Set to true when fPermSem has been initialised.
	bool               fPermSemReady { false };
};

} // namespace gui

#endif // HAIKU_CLAUDE_CLI_GUI_SINK_H
