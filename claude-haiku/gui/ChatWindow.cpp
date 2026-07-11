// GUI front-end implementation. See ChatWindow.h for the threading model:
// a worker thread runs the agent turn and posts MSG_* messages back here,
// where MessageReceived (main thread, window lock held) mutates the BTextView.

#include "gui/ChatWindow.h"

#include <cstring>
#include <initializer_list>

#include <Alignment.h>
#include <Button.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <TextControl.h>
#include <TextView.h>

static const BRect kInitialFrame(100, 100, 700, 500);

ChatWindow::ChatWindow()
	:
	// B_AUTO_UPDATE_SIZE_LIMITS is intentionally omitted: we call
	// SetSizeLimits ourselves after the first layout pass so the window
	// can never be resized narrow enough to hide the button column.
	BWindow(kInitialFrame, "Claude", B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE),
	fOutput(nullptr),
	fInput(nullptr),
	fSend(nullptr),
	fClear(nullptr),
	fSettings(nullptr),
	fWorker(-1)
{
	fOutput = new BTextView("output");
	fOutput->MakeEditable(false);
	fOutput->SetStylable(true);
	fOutput->SetWordWrap(true);
	BScrollView* scroll = new BScrollView("scroll", fOutput,
		0, false, true, B_FANCY_BORDER);

	fInput = new BTextControl("input", nullptr, "", new BMessage(MSG_SEND));

	fSend    = new BButton("send",     "Send",     new BMessage(MSG_SEND));
	fClear   = new BButton("clear",    "Clear",    new BMessage(MSG_CLEAR));
	fSettings= new BButton("settings", "Settings", new BMessage(MSG_SETTINGS));

	// Input expands horizontally but stays one line tall.
	fInput->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, fInput->MinSize().height));

	// Each button takes only the width its own label needs (no stretching to
	// a uniform column width) and hugs the right edge of the column so the
	// stack is right-aligned to the window.
	for (BButton* b : {fSend, fClear, fSettings})
		b->SetExplicitAlignment(BAlignment(B_ALIGN_RIGHT, B_ALIGN_VERTICAL_UNSET));

	// ┌──────────────────────────────┐
	// │  scroll (weight 1)           │
	// ├───────────────────┬──────────┤
	// │  input (weight 1) │  Send    │
	// │                   │  Clear   │
	// │                   │  Settings│
	// └───────────────────┴──────────┘
	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(scroll, 1)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(fInput, 1)
			.AddGroup(B_VERTICAL, 0)
				.Add(fSend)
				.Add(fClear)
				.Add(fSettings)
			.End()
		.End();

	fSend->MakeDefault(true);
	fInput->MakeFocus(true);

	// Force a layout pass now so MinSize() reflects real font metrics,
	// then lock the window's minimum size to prevent the buttons being
	// squeezed off the right edge.
	Layout(true);
	BSize minSize = GetLayout()->MinSize();
	SetSizeLimits(minSize.width, B_SIZE_UNLIMITED,
		minSize.height, B_SIZE_UNLIMITED);
}

void
ChatWindow::MessageReceived(BMessage* msg)
{
	switch (msg->what) {
		case MSG_SEND:
			_StartTurn();
			break;

		case MSG_CHUNK:
		{
			const char* text = nullptr;
			if (msg->FindString("text", &text) == B_OK)
				_Append(text);
			break;
		}

		case MSG_DONE:
			fWorker = -1;
			break;

		case MSG_ERR:
		{
			const char* text = nullptr;
			if (msg->FindString("text", &text) == B_OK)
				_Append(text);
			fWorker = -1;
			break;
		}

		case MSG_CLEAR:
			_Clear();
			break;

		case MSG_SETTINGS:
			// TODO: open settings panel.
			break;

		default:
			BWindow::MessageReceived(msg);
			break;
	}
}

bool
ChatWindow::QuitRequested()
{
	return true;
}

void
ChatWindow::_StartTurn()
{
	BString prompt(fInput->Text());
	prompt.Trim();
	if (prompt.IsEmpty())
		return;

	_Append(prompt.String());
	_Append("\n");
	fInput->SetText("");

	// TODO: spawn the worker thread that runs cch::AgentLoop::Turn and posts
	// MSG_CHUNK / MSG_DONE / MSG_ERR back to this window.
}

void
ChatWindow::_Append(const char* text)
{
	if (text == nullptr)
		return;
	fOutput->Insert(fOutput->TextLength(), text, strlen(text));
	fOutput->ScrollToOffset(fOutput->TextLength());
}

void
ChatWindow::_Clear()
{
	fOutput->SetText("");
}
