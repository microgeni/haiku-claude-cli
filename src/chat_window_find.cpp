// chat_window_find.cpp — ChatWindow's "find in conversation" (Cmd-F)
// methods, split out of chat_window.cpp for navigability. These implement
// ChatWindow:: members declared in chat_window.h; they share the class's
// private state like any other ChatWindow method.

#include "chat_window.h"

#include <cctype>
#include <string>
#include <vector>

#include <SplitView.h>
#include <TextControl.h>
#include <TextView.h>

// ---------------------------------------------------------------------------
// Find in conversation (Cmd-F)
// ---------------------------------------------------------------------------

void ChatWindow::_ToggleFindBar()
{
	if (!fFindBar) return;
	if (fFindBar->IsHidden()) {
		// The find bar lives inside the input pane; if that pane is collapsed
		// (View > Input), revealing the find bar alone would draw nothing.
		// Show the pane first so the find bar is actually visible.
		if (fInputPane && fInputPane->IsHidden()) {
			fInputPane->Show();
			if (fInputItem) fInputItem->SetMarked(true);
			if (fVSplit) {
				fVSplit->InvalidateLayout(true);
				fVSplit->Relayout();
			}
		}
		fFindBar->Show();
		if (fFindField) {
			// Pre-fill with the current selection, if any, then focus.
			int32 selStart = 0, selEnd = 0;
			fOutput->GetSelection(&selStart, &selEnd);
			if (selEnd > selStart && (selEnd - selStart) < 128) {
				std::string sel(fOutput->Text() + selStart, selEnd - selStart);
				fFindField->SetText(sel.c_str());
			}
			fFindField->MakeFocus(true);
			if (BTextView* tv = fFindField->TextView())
				tv->SelectAll();
		}
		fFindMatchStart = -1;
	} else {
		fFindBar->Hide();
		fInput->MakeFocus(true);
	}
}

// Select and scroll to the next (forward) or previous match of the find
// field's text, searching case-insensitively over the chat output. Wraps
// around the ends. Updates the "n / total" counter.
void ChatWindow::_FindNext(bool forward)
{
	if (!fFindField || !fOutput) return;
	const char* raw = fFindField->Text();
	const std::string needle = raw ? raw : "";
	if (needle.empty()) {
		if (fFindStatus) fFindStatus->SetText("");
		return;
	}

	// Case-insensitive haystack/needle copies.
	auto lower = [](std::string s) {
		for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
		return s;
	};
	const std::string hay = lower(fOutput->Text());
	const std::string ndl = lower(needle);

	// Count all matches and find the current/next one in the chosen
	// direction relative to fFindMatchStart.
	std::vector<size_t> matches;
	for (size_t p = hay.find(ndl); p != std::string::npos;
	     p = hay.find(ndl, p + 1))
		matches.push_back(p);

	if (matches.empty()) {
		if (fFindStatus) fFindStatus->SetText("not found");
		fFindMatchStart = -1;
		return;
	}

	// Pick the target index relative to the current match (wraps).
	int target = 0;
	if (fFindMatchStart < 0) {
		target = forward ? 0 : static_cast<int>(matches.size()) - 1;
	} else {
		// Locate the current match's index.
		int cur = 0;
		for (size_t i = 0; i < matches.size(); ++i)
			if (static_cast<int32>(matches[i]) == fFindMatchStart) { cur = static_cast<int>(i); break; }
		const int n = static_cast<int>(matches.size());
		target = forward ? (cur + 1) % n : (cur - 1 + n) % n;
	}

	const size_t start = matches[target];
	const int32  s = static_cast<int32>(start);
	const int32  e = static_cast<int32>(start + ndl.size());
	fFindMatchStart = s;

	fOutput->Select(s, e);
	fOutput->ScrollToSelection();

	if (fFindStatus) {
		const std::string label = std::to_string(target + 1) + " / "
		                        + std::to_string(matches.size());
		fFindStatus->SetText(label.c_str());
	}
}
