#include "gui_widgets.h"

#include <algorithm>
#include <cstdio>

#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>

#include "gui_messages.h"
#include "gui_scale.h"

// ---------------------------------------------------------------------------
// ChoiceModal
// ---------------------------------------------------------------------------

ChoiceModal::ChoiceModal(const std::string& prompt,
                         const std::vector<std::string>& options)
	: BWindow(BRect(0, 0, 360, 100), "Choose",
	          B_MODAL_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
	          B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
	  fDoneSem(create_sem(0, "choice_modal"))
{
	BStringView* label = new BStringView("prompt", prompt.c_str());
	label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	BLayoutBuilder::Group<> builder(this, B_VERTICAL, B_USE_SMALL_SPACING);
	builder.SetInsets(B_USE_WINDOW_SPACING);
	builder.Add(label);
	BButton* first = nullptr;
	for (size_t i = 0; i < options.size(); ++i) {
		BMessage* m = new BMessage('CHpk');
		m->AddInt32("index", static_cast<int32>(i));
		BButton* b = new BButton("opt", options[i].c_str(), m);
		b->SetTarget(this);
		if (i == 0) first = b;
		builder.Add(b);
	}

	// Keyboard a11y: Enter activates the first option, Esc cancels.
	if (first) {
		first->MakeDefault(true);
		first->MakeFocus(true);
	}
	AddShortcut(B_ESCAPE, 0, new BMessage('CHcl'));
	CenterOnScreen();
}

ChoiceModal::~ChoiceModal() { delete_sem(fDoneSem); }

void ChoiceModal::MessageReceived(BMessage* msg)
{
	if (msg->what == 'CHpk') {
		int32 idx = -1;
		msg->FindInt32("index", &idx);
		fResult = idx;
		_Finish();
		return;
	}
	if (msg->what == 'CHcl') {   // Esc — cancel
		fResult = -1;
		_Finish();
		return;
	}
	BWindow::MessageReceived(msg);
}

bool ChoiceModal::QuitRequested()
{
	// Closing the window without a pick counts as cancel.
	_Finish();
	return true;
}

int ChoiceModal::Go()
{
	Show();
	// Block the caller; the window's looper runs MessageReceived on its
	// own thread and releases the semaphore when a choice is made.
	acquire_sem(fDoneSem);
	const int r = fResult;
	if (Lock()) Quit();   // tears down the window + looper
	return r;
}

void ChoiceModal::_Finish()
{
	if (!fFinished) {
		fFinished = true;
		release_sem(fDoneSem);
	}
}

// ---------------------------------------------------------------------------
// SessionItem
// ---------------------------------------------------------------------------

SessionItem::SessionItem(const std::string& label, const std::string& path,
                         const std::string& title)
	: BStringItem(label.c_str()), fPath(path), fTitle(title) {}

// ---------------------------------------------------------------------------
// RenameModal
// ---------------------------------------------------------------------------

RenameModal::RenameModal(const std::string& current, const char* title,
                         const char* label, const char* okLabel,
                         float widthPx)
	: BWindow(BRect(0, 0, widthPx, 90), title,
	          B_MODAL_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
	          B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
	  fDoneSem(create_sem(0, "rename_modal"))
{
	fField = new BTextControl("name", label, current.c_str(),
	                          new BMessage('RNok'));
	fField->SetTarget(this);
	BButton* ok     = new BButton("ok", okLabel, new BMessage('RNok'));
	BButton* cancel = new BButton("cancel", "Cancel", new BMessage('RNcl'));
	ok->MakeDefault(true);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fField)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.AddGlue()
			.Add(cancel)
			.Add(ok)
		.End()
	.End();

	AddShortcut(B_ESCAPE, 0, new BMessage('RNcl'));
	CenterOnScreen();
}

RenameModal::~RenameModal() { delete_sem(fDoneSem); }

void RenameModal::Show()
{
	BWindow::Show();
	if (Lock()) {
		fField->MakeFocus(true);
		if (BTextView* tv = fField->TextView()) tv->SelectAll();
		Unlock();
	}
}

void RenameModal::MessageReceived(BMessage* msg)
{
	if (msg->what == 'RNok') {
		if (const char* t = fField->Text()) fResult = t;
		fAccepted = true;
		_Finish();
		return;
	}
	if (msg->what == 'RNcl') { fResult.clear(); fAccepted = false; _Finish(); return; }
	BWindow::MessageReceived(msg);
}

// Closing the window (title-bar close) is a cancel, not a confirm — leave
// fAccepted false so callers that accept empty input don't act on it.
bool RenameModal::QuitRequested() { _Finish(); return true; }

std::string RenameModal::Go(bool* accepted)
{
	Show();
	acquire_sem(fDoneSem);
	// Read both results BEFORE Quit() — it deletes this window.
	const std::string r  = fResult;
	const bool        ok = fAccepted;
	if (Lock()) Quit();
	if (accepted) *accepted = ok;
	return r;
}

void RenameModal::_Finish()
{
	if (!fFinished) { fFinished = true; release_sem(fDoneSem); }
}

// ---------------------------------------------------------------------------
// SessionListView
// ---------------------------------------------------------------------------

SessionListView::SessionListView(const char* name, BHandler* target)
	: BListView(name, B_MULTIPLE_SELECTION_LIST), fTarget(target) {}

void SessionListView::MouseDown(BPoint where)
{
	uint32 buttons = 0;
	if (Window()) {
		BMessage* msg = Window()->CurrentMessage();
		if (msg) msg->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
	}

	if (buttons & B_SECONDARY_MOUSE_BUTTON) {
		const int32 idx = IndexOf(where);
		if (idx < 0) { BListView::MouseDown(where); return; }
		// Right-clicking a row that isn't part of the current selection
		// makes it the sole selection (Tracker-like).
		if (!IsItemSelected(idx)) Select(idx);

		BPopUpMenu* menu = new BPopUpMenu("ctx", false, false);
		menu->AddItem(new BMenuItem("Rename\xE2\x80\xA6",
			new BMessage(gui::MSG_SESSION_RENAME)));
		menu->AddItem(new BMenuItem("Open",
			new BMessage(gui::MSG_SESSION_SELECT)));
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem("Delete",
			new BMessage(gui::MSG_SESSION_DELETE)));
		menu->SetTargetForItems(fTarget);

		BPoint screenPt = where;
		ConvertToScreen(&screenPt);
		menu->Go(screenPt, true, true, true);
		return;
	}
	BListView::MouseDown(where);
}

// ---------------------------------------------------------------------------
// SessionWindow
// ---------------------------------------------------------------------------

SessionWindow::SessionWindow(BWindow* owner, BHandler* target)
	: BWindow(BRect(0, 0, gui::ScalePx(320), gui::ScalePx(440)),
	          "Sessions", B_FLOATING_WINDOW_LOOK, B_FLOATING_SUBSET_WINDOW_FEEL,
	          B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
	  fOwner(owner)
{
	// A floating subset window stays above its owner and follows it between
	// workspaces without stealing the app's quit behaviour.
	if (owner) AddToSubset(owner);

	fList = new SessionListView("sessionlist", target);
	fList->SetSelectionMessage(nullptr);              // single-click highlights
	fList->SetInvocationMessage(                      // double-click loads
		new BMessage(gui::MSG_SESSION_SELECT));

	BScrollView* scroll = new BScrollView("sessionscroll", fList,
	                                      0, false, true, B_FANCY_BORDER);
	scroll->SetExplicitMinSize(BSize(gui::ScalePx(120), gui::ScalePx(120)));

	BButton* sessNew = new BButton("sessnew", "New",
	                               new BMessage(gui::MSG_SESSION_NEW));
	BButton* sessOpen = new BButton("sessopen", "Open",
	                                new BMessage(gui::MSG_SESSION_SELECT));
	BButton* sessRen = new BButton("sessren", "Rename",
	                               new BMessage(gui::MSG_SESSION_RENAME));
	BButton* sessDel = new BButton("sessdel", "Delete",
	                               new BMessage(gui::MSG_SESSION_DELETE));
	for (BButton* b : { sessNew, sessOpen, sessRen, sessDel })
		b->SetTarget(target);

	// Uniform button width so the two rows line up as a tidy grid.
	float btnW = 0.0f;
	for (BButton* b : { sessNew, sessOpen, sessRen, sessDel })
		btnW = std::max(btnW, b->PreferredSize().Width());
	for (BButton* b : { sessNew, sessOpen, sessRen, sessDel })
		b->SetExplicitSize(BSize(btnW, B_SIZE_UNSET));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_INSETS)
		.Add(scroll, 1.0f)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(sessNew, 0.0f)
			.Add(sessOpen, 0.0f)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(sessRen, 0.0f)
			.Add(sessDel, 0.0f)
			.AddGlue()
		.End()
	.End();

	SetSizeLimits(gui::ScalePx(200), gui::ScalePx(600),
	              gui::ScalePx(160), 100000);
}

bool SessionWindow::QuitRequested()
{
	// Don't destroy — just tuck the window away so its list survives and
	// View ▸ Sessions can bring it straight back.
	if (!IsHidden()) Hide();
	return false;
}

void SessionWindow::ShowNear(BWindow* owner)
{
	if (!fPlaced && owner) {
		// Dock just to the left of the owning window on first reveal.
		BRect of = owner->Frame();
		float w = Frame().Width();
		float x = of.left - w - gui::ScalePx(12);
		if (x < 0) x = of.right + gui::ScalePx(12);   // no room left → go right
		MoveTo(x, of.top);
		fPlaced = true;
	}
	if (IsHidden()) Show();
	Activate(true);
}

// ---------------------------------------------------------------------------
// NotifySlider
// ---------------------------------------------------------------------------

NotifySlider::NotifySlider(const char* name, const char* label,
                           BMessage* message, int32 minValue, int32 maxValue)
	: BSlider(name, label, message, minValue, maxValue, B_HORIZONTAL)
{
}

void NotifySlider::SetValueLabel(BStringView* label)
{
	fLabel = label;
	_UpdateLabel();
}

void NotifySlider::SetValue(int32 value)
{
	int32 snapped = ((value + 5) / 10) * 10;
	BSlider::SetValue(snapped);
	_UpdateLabel();
}

const char* NotifySlider::UpdateText() const
{
	return nullptr;
}

void NotifySlider::FormatNotifyDelay(int32 seconds, char* out, size_t outSize)
{
	if (seconds <= 0) {
		std::snprintf(out, outSize, "Notify is disabled");
		return;
	}
	const int mins = static_cast<int>(seconds) / 60;
	const int secs = static_cast<int>(seconds) % 60;
	if (mins == 0)
		std::snprintf(out, outSize, "Notify after %ds", secs);
	else if (secs == 0)
		std::snprintf(out, outSize, "Notify after %d %s",
		              mins, mins == 1 ? "minute" : "minutes");
	else
		std::snprintf(out, outSize, "Notify after %d min %ds", mins, secs);
}

void NotifySlider::_UpdateLabel()
{
	if (!fLabel) return;
	char text[40];
	FormatNotifyDelay(Value(), text, sizeof(text));
	fLabel->SetText(text);
}
