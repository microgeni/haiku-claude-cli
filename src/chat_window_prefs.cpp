// chat_window_prefs.cpp — ChatWindow's persisted GUI-preferences load/save
// (window frame, chat zoom, last-used model), split out of chat_window.cpp
// for navigability. The prefs live in a flattened BMessage at
// paths::GuiPrefsPath(). These implement ChatWindow:: members declared in
// chat_window.h and share the class's private state like any other method.

#include "chat_window.h"

#include <string>

#include <File.h>
#include <Message.h>
#include <Screen.h>
#include <SplitView.h>

#include "config.h"
#include "paths.h"

// Global GUI preferences — a flattened BMessage at paths::GuiPrefsPath().
// ---------------------------------------------------------------------------

void ChatWindow::_LoadGuiPrefs()
{
	BFile file(paths::GuiPrefsPath().c_str(), B_READ_ONLY);
	if (file.InitCheck() != B_OK) return;
	BMessage prefs;
	if (prefs.Unflatten(&file) != B_OK) return;

	// Window frame — clamp into the current screen so a prefs file from
	// a larger display doesn't park the window off-screen. This matters
	// whenever the settings directory is shared between machines: a frame
	// saved on a 5120x2160 desktop leaves the window invisible on a 1080p
	// screen, with the app still running and listed in the Deskbar.
	BRect frame;
	if (prefs.FindRect("frame", &frame) == B_OK && frame.IsValid()
			&& frame.Width() > 200 && frame.Height() > 150) {
		// Clamp to the window minimum so a small saved frame can't start
		// the window below the floor that keeps the input bar visible.
		float rw = frame.Width();
		float rh = frame.Height();
		if (rw < fWindowMinW) rw = fWindowMinW;
		if (rh < fWindowMinH) rh = fWindowMinH;

		BPoint origin = frame.LeftTop();

		BScreen screen(this);
		const BRect bounds = screen.Frame();
		if (bounds.IsValid()) {
			// Leave the title tab reachable: a window placed flush with
			// the screen top can't be dragged back into view.
			const float kTabAllowance = 24.0f;
			const float usableTop = bounds.top + kTabAllowance;

			// Never ask for more room than the screen actually has. The
			// usable height excludes the tab allowance, otherwise pushing
			// the origin down below would run the window off the bottom.
			if (rw > bounds.Width())          rw = bounds.Width();
			if (rh > bounds.bottom - usableTop) rh = bounds.bottom - usableTop;

			if (origin.x + rw > bounds.right)  origin.x = bounds.right - rw;
			if (origin.y + rh > bounds.bottom) origin.y = bounds.bottom - rh;
			if (origin.x < bounds.left) origin.x = bounds.left;
			if (origin.y < usableTop)   origin.y = usableTop;
		}

		MoveTo(origin);
		ResizeTo(rw, rh);
	}

	float zoom = 1.0f;
	if (prefs.FindFloat("zoom", &zoom) == B_OK
			&& zoom >= 0.6f && zoom <= 2.5f) {
		fZoomFactor  = zoom;
		fAppliedZoom = zoom;   // empty buffer; new text arrives pre-scaled
		if (fMdRenderer) fMdRenderer->SetZoom(zoom);
	}

	// Restore the splitter weight of the sidebar relative to the chat.
	if (fSplit) {
		float sidebarWeight = 0.0f;
		if (prefs.FindFloat("sidebar_weight", &sidebarWeight) == B_OK
				&& sidebarWeight > 0.0f && sidebarWeight < 5.0f)
			fSplit->SetItemWeight((int32)0, sidebarWeight, false);
		fSplit->SetItemWeight((int32)1, 1.0f, true);
	}

	// Restore the splitter weight of the chat relative to the input pane.
	if (fVSplit) {
		float inputWeight = 0.0f;
		if (prefs.FindFloat("input_weight", &inputWeight) == B_OK
				&& inputWeight > 0.0f && inputWeight < 5.0f)
			fVSplit->SetItemWeight((int32)1, inputWeight, false);
		fVSplit->SetItemWeight((int32)0, 1.0f, true);
	}

	// Last-used model — only adopt the auto-saved GUI model when the user
	// hasn't explicitly pinned one in config.json / via a CLI flag. If the
	// constructor model differs from the built-in default, that value was
	// deliberately chosen and must win over the saved last-used model.
	// A loaded session still overrides this later.
	const bool userPinnedModel =
		!fConfigModel.empty() && fConfigModel != config::kDefaultModel;
	const char* model = nullptr;
	if (!userPinnedModel
			&& prefs.FindString("model", &model) == B_OK && model && model[0]) {
		fModel = model;
		_UpdateTitle();
		if (fModelMenu) {
			// Menu lives in the settings dialog looper — lock it.
			const bool dlgLocked = fSettings && fSettings->Lock();
			for (int32 i = 0; i < fModelMenu->CountItems(); ++i) {
				BMenuItem* it = fModelMenu->ItemAt(i);
				if (it) it->SetMarked(it->Label() && fModel == it->Label());
			}
			if (dlgLocked) fSettings->Unlock();
		}
	}
}

void ChatWindow::_SaveGuiPrefs()
{
	paths::MkdirP(paths::ConfigDir());
	BMessage prefs('GPRF');
	prefs.AddRect("frame", Frame());
	prefs.AddFloat("zoom", fZoomFactor);
	prefs.AddString("model", fModel.c_str());
	// Splitter weight (relative width of the sidebar).
	if (fSplit) {
		prefs.AddFloat("sidebar_weight",  fSplit->ItemWeight((int32)0));
	}
	// Splitter weight (relative height of the input pane).
	if (fVSplit) {
		prefs.AddFloat("input_weight",  fVSplit->ItemWeight((int32)1));
	}

	BFile file(paths::GuiPrefsPath().c_str(),
	           B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		prefs.Flatten(&file);
}
