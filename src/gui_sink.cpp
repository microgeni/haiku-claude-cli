#include "gui_sink.h"

#include <cstring>
#include <string>

#include <Message.h>

namespace gui {

GuiSink::GuiSink(BMessenger window)
	: fWindow(window)
{
	fPermSemReady = (sem_init(&fPermSem, 0, 0) == 0);
}

GuiSink::~GuiSink()
{
	if (fPermSemReady) {
		sem_destroy(&fPermSem);
		fPermSemReady = false;
	}
}

// ── StructuredSink methods ────────────────────────────────────────────────────

void GuiSink::BeginMessage(const std::string& /*role*/)
{
	// Header ("claude ▸") is written directly by ChatWindow::_SendTurn()
	// before the worker starts. Nothing to do here.
}

void GuiSink::AppendText(const std::string& chunk)
{
	BMessage msg(MSG_CHUNK);
	msg.AddString("text", chunk.c_str());
	fWindow.SendMessage(&msg);
}

void GuiSink::EndMessage()
{
	// Append a trailing newline so the next turn's header starts cleanly.
	BMessage msg(MSG_CHUNK);
	msg.AddString("text", "\n");
	fWindow.SendMessage(&msg);
	fWindow.SendMessage(MSG_DONE);
}

void GuiSink::ToolStarted(const std::string& name, const std::string& summary)
{
	BMessage msg(MSG_TOOL_START);
	msg.AddString("name",    name.c_str());
	msg.AddString("summary", summary.c_str());
	fWindow.SendMessage(&msg);
}

void GuiSink::ToolFinished(const std::string& name, bool ok,
                            const std::string& /*detail*/)
{
	BMessage msg(MSG_TOOL_DONE);
	msg.AddString("name", name.c_str());
	msg.AddBool("ok",     ok);
	fWindow.SendMessage(&msg);
}

int GuiSink::AskChoice(const std::string& /*prompt*/,
                        const std::vector<std::string>& options)
{
	// Step 4: no interactive choice UI yet. Return the first option
	// (index 0) so the conversation can continue. A proper BAlert with
	// buttons is Step 5.
	(void)options;
	return 0;
}

sink::Permission GuiSink::AskPermission(const std::string& tool,
                                         const std::string& preview)
{
	if (!fPermSemReady) return sink::Permission::kDeny;

	// Post the question to the main thread (non-blocking).
	BMessage msg(MSG_ASK_PERM);
	msg.AddString("tool",    tool.c_str());
	msg.AddString("preview", preview.c_str());
	fWindow.SendMessage(&msg);

	// Block until ChatWindow::MessageReceived calls DeliverPermissionReply.
	sem_wait(&fPermSem);

	return fPermResult.load()
		? sink::Permission::kAllow
		: sink::Permission::kDeny;
}

void GuiSink::SetStatus(sink::StatusKind kind)
{
	BMessage msg(MSG_STATUS);
	msg.AddInt32("kind", static_cast<int32>(kind));
	fWindow.SendMessage(&msg);
}

void GuiSink::OnError(const std::string& message)
{
	BMessage msg(MSG_ERR);
	msg.AddString("text", message.c_str());
	fWindow.SendMessage(&msg);
}

// ── OutputSink adapter methods ────────────────────────────────────────────────

void GuiSink::OnText(const std::string& chunk)
{
	AppendText(chunk);
}

void GuiSink::OnToolStatus(const std::string& phase)
{
	if (phase.empty()) {
		// Tool finished — find a plain name from the phase string.
		ToolFinished("tool", true, {});
	} else {
		// Extract name from "🔧 running <Name>…" pattern.
		const std::string prefix = " running ";
		std::string name = phase;
		const auto p = phase.find(prefix);
		if (p != std::string::npos) {
			name = phase.substr(p + prefix.size());
			while (!name.empty() && (unsigned char)name.back() > 127)
				name.pop_back(); // strip trailing UTF-8 ellipsis
		}
		ToolStarted(name, "running");
	}
}

api::Permission GuiSink::AskPermission(const std::string& tool_name,
                                        const nlohmann::json& input,
                                        std::string* denial_reason)
{
	// Build a plain-text preview from the tool input (no ANSI codes —
	// the preview goes into a BAlert which renders plain text only).
	std::string preview;
	if (input.contains("command") && input["command"].is_string())
		preview = input["command"].get<std::string>();
	else if (input.contains("path") && input["path"].is_string())
		preview = input["path"].get<std::string>();
	else
		preview = input.dump(-1, ' ', false,
			nlohmann::json::error_handler_t::replace).substr(0, 300);

	const sink::Permission p = AskPermission(tool_name, preview);
	switch (p) {
		case sink::Permission::kAllow:
		case sink::Permission::kAllowAlways:
			return api::Permission::Allow;
		case sink::Permission::kDeny:
			if (denial_reason)
				*denial_reason = "user denied permission via GUI dialog";
			return api::Permission::Deny;
	}
	return api::Permission::Deny;
}

void GuiSink::DeliverPermissionReply(bool granted)
{
	fPermResult.store(granted);
	if (fPermSemReady) sem_post(&fPermSem);
}

} // namespace gui
