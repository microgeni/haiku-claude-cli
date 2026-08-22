#include "gui_sink.h"

#include <cstring>
#include <string>

#include <Message.h>

#include "api.h"
#include "tools.h"

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

// An advisory the user should see. Unlike OnMeta/OnDiag this is not
// suppressed — it goes to the chat view as a tool-log line.
void GuiSink::OnNotice(const std::string& text)
{
	if (text.empty()) return;
	BMessage msg(MSG_NOTICE);
	msg.AddString("text", text.c_str());
	fWindow.SendMessage(&msg);
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

int GuiSink::AskChoice(const std::string& prompt,
                        const std::vector<std::string>& options)
{
	if (options.empty()) return -1;
	if (!fPermSemReady)  return -1;

	// Post the prompt and options to the main thread (non-blocking),
	// then block on fPermSem until ChatWindow shows a modal and calls
	// DeliverChoiceReply(). Mirrors the AskPermission handshake; the two
	// never overlap because both run on the single worker thread.
	BMessage msg(MSG_ASK_CHOICE);
	msg.AddString("prompt", prompt.c_str());
	for (const auto& opt : options)
		msg.AddString("options", opt.c_str());
	fWindow.SendMessage(&msg);

	sem_wait(&fPermSem);
	return fChoiceResult.load();
}

sink::Permission GuiSink::AskPermission(const std::string& tool,
                                         const std::string& preview)
{
	// Ludicrous mode: skip the alert entirely.
	if (api::g_ludicrous_mode.load()) return sink::Permission::kAllow;

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

void GuiSink::OnThinking(const std::string& chunk)
{
	BMessage msg(MSG_THINKING);
	msg.AddString("text", chunk.c_str());
	fWindow.SendMessage(&msg);
}

void GuiSink::OnToolStatus(const std::string& phase)
{
	if (phase.empty()) {
		// Tool finished — find a plain name from the phase string.
		ToolFinished("tool", true, {});
	} else {
		// Extract name (and optional ": args") from the
		// "🔧 running <Name>: <args>…" pattern produced by api.cpp.
		const std::string prefix = " running ";
		std::string name = phase;
		std::string args;
		const auto p = phase.find(prefix);
		if (p != std::string::npos) {
			name = phase.substr(p + prefix.size());
			// Strip the trailing UTF-8 ellipsis (… == \xE2\x80\xA6) that
			// api.cpp appends — exactly that suffix, so a command ending in
			// a multibyte UTF-8 character isn't corrupted.
			const std::string kEllipsis = "\xE2\x80\xA6";
			if (name.size() >= kEllipsis.size()
			    && name.compare(name.size() - kEllipsis.size(),
			                    kEllipsis.size(), kEllipsis) == 0)
				name.resize(name.size() - kEllipsis.size());
			// Split "<Name>: <args>" into name and args.
			const auto colon = name.find(": ");
			if (colon != std::string::npos) {
				args = name.substr(colon + 2);
				name.resize(colon);
			}
		}
		ToolStarted(name, args.empty() ? "running" : args);
	}
}

api::Permission GuiSink::AskPermission(const std::string& tool_name,
                                        const nlohmann::json& input,
                                        std::string* denial_reason)
{
	// Tools that are always safe need no dialog.
	if (api::AlwaysAllowed().count(tool_name)) return api::Permission::Allow;
	if (!tools::RequiresPermission(tool_name, input))  return api::Permission::Allow;

	// Send a structured diff to the chat window before asking (or auto-approving).
	// This fires for Edit and Write regardless of ludicrous mode so the user
	// always sees what changed in the output area.
	{
		const std::string diff = tools::GuiDiff(tool_name, input);
		if (!diff.empty()) {
			BMessage diffMsg(MSG_TOOL_DIFF);
			diffMsg.AddString("diff", diff.c_str());
			fWindow.SendMessage(&diffMsg);
		}
	}

	// Ludicrous mode: skip the alert entirely.
	if (api::g_ludicrous_mode.load()) return api::Permission::Allow;

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

void GuiSink::DeliverChoiceReply(int index)
{
	fChoiceResult.store(index);
	if (fPermSemReady) sem_post(&fPermSem);
}

} // namespace gui
