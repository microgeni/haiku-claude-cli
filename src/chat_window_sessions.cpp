// chat_window_sessions.cpp — ChatWindow's session-sidebar methods
// (toggle / refresh / load / delete / rename), split out of chat_window.cpp
// for navigability. These implement ChatWindow:: members declared in
// chat_window.h and share the class's private state like any other method.

#include "chat_window.h"

#include <string>
#include <vector>

#include <Alert.h>
#include <ListView.h>
#include <ScrollView.h>
#include <SplitView.h>

#include "gui_widgets.h"     // SessionItem, RenameModal, SessionListView
#include "session_store.h"

// ---------------------------------------------------------------------------
// Session sidebar
// ---------------------------------------------------------------------------

void ChatWindow::_ToggleSessionList()
{
	if (!fSessionPanel) return;
	if (fSessionPanel->IsHidden()) {
		_RefreshSessionList();
		fSessionPanel->Show();
	} else {
		fSessionPanel->Hide();
	}
}

// Repopulate the list from the BFS session store, newest first. Marks the
// row matching the currently-open session (fSessionPath) as selected.
void ChatWindow::_RefreshSessionList()
{
	if (!fSessionList) return;

	// Clear existing items.
	for (int32 i = fSessionList->CountItems() - 1; i >= 0; --i)
		delete fSessionList->RemoveItem(i);

	const std::vector<session::SessionInfo> sessions = session::List();
	int32 selectIdx = -1;
	for (size_t i = 0; i < sessions.size(); ++i) {
		const session::SessionInfo& s = sessions[i];
		std::string label = s.title.empty() ? "(untitled)" : s.title;
		if (s.turns > 0) label += "  (" + std::to_string(s.turns) + ")";
		fSessionList->AddItem(new SessionItem(label, s.path,
			s.title.empty() ? "" : s.title));
		if (!fSessionPath.empty() && s.path == fSessionPath)
			selectIdx = static_cast<int32>(i);
	}
	if (selectIdx >= 0) fSessionList->Select(selectIdx);
}

void ChatWindow::_LoadSelectedSession()
{
	if (!fSessionList) return;
	// Loading is a single-session action. If several rows are selected
	// (multi-select for bulk delete), don't guess — require exactly one.
	if (fSessionList->CountItems() == 0) return;
	int32 selCount = 0;
	int32 only     = -1;
	for (int32 i = 0; i < fSessionList->CountItems(); ++i) {
		if (fSessionList->IsItemSelected(i)) { ++selCount; only = i; }
	}
	if (selCount != 1) return;
	SessionItem* item = dynamic_cast<SessionItem*>(fSessionList->ItemAt(only));
	if (!item) return;
	// _LoadSession replays the conversation and sets fSessionPath.
	_LoadSession(item->Path());
}

void ChatWindow::_DeleteSelectedSession()
{
	if (!fSessionList) return;

	// Collect the paths of every selected row (multi-select).
	std::vector<std::string> paths;
	for (int32 i = 0; i < fSessionList->CountItems(); ++i) {
		if (!fSessionList->IsItemSelected(i)) continue;
		SessionItem* item = dynamic_cast<SessionItem*>(fSessionList->ItemAt(i));
		if (item) paths.push_back(item->Path());
	}
	if (paths.empty()) return;

	// Confirm, with the count when more than one is selected.
	std::string msg = (paths.size() == 1)
		? "Delete this saved session? This cannot be undone."
		: "Delete " + std::to_string(paths.size())
		  + " saved sessions? This cannot be undone.";
	BAlert* confirm = new BAlert("Delete sessions", msg.c_str(),
	    "Cancel", "Delete", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	confirm->SetShortcut(0, B_ESCAPE);
	if (confirm->Go() != 1) return;   // 0 = Cancel

	for (const std::string& path : paths) {
		if (session::Delete(path)) {
			// If we deleted the open session, detach so the next save
			// makes a fresh file rather than recreating the deleted one.
			if (fSessionPath == path) fSessionPath.clear();
		}
	}
	_RefreshSessionList();
}

void ChatWindow::_RenameSelectedSession()
{
	if (!fSessionList) return;
	// Rename is a single-row action — require exactly one selection.
	int32 selCount = 0, only = -1;
	for (int32 i = 0; i < fSessionList->CountItems(); ++i)
		if (fSessionList->IsItemSelected(i)) { ++selCount; only = i; }
	if (selCount != 1) return;

	SessionItem* item = dynamic_cast<SessionItem*>(fSessionList->ItemAt(only));
	if (!item) return;

	const std::string current = item->Title();
	RenameModal* modal = new RenameModal(current);
	const std::string entered = modal->Go();   // self-quits

	// Trim whitespace; empty / unchanged → no-op.
	std::string name = entered;
	while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
		name.erase(name.begin());
	while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
		name.pop_back();
	if (name.empty() || name == current) return;

	if (session::Rename(item->Path(), name)) {
		// If we renamed the currently-open session, keep fConvTopic in
		// sync so the next auto-save doesn't overwrite the new title.
		if (item->Path() == fSessionPath) fConvTopic = name;
		_RefreshSessionList();
	}
}

// ---------------------------------------------------------------------------
