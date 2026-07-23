#ifndef HAIKU_CLAUDE_CLI_GUI_WIDGETS_H
#define HAIKU_CLAUDE_CLI_GUI_WIDGETS_H

#include <cstddef>
#include <string>
#include <vector>

#include <Button.h>
#include <ListItem.h>
#include <ListView.h>
#include <Slider.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

// gui_widgets — small, self-contained BeAPI helper widgets used by the
// chat window. Each communicates with the owning window only through the
// shared gui::MSG_* codes (gui_messages.h) or a BHandler* target, never by
// holding a ChatWindow pointer, so they live in their own translation unit.

// ChoiceModal — a modal window presenting one button per option, used when
// there are more than three choices (BAlert caps at three buttons). Runs a
// nested wait via a semaphore so the caller blocks until the user picks.
// Go() returns the 0-based chosen index, or -1 if closed without a pick.
class ChoiceModal : public BWindow {
public:
	ChoiceModal(const std::string& prompt,
	            const std::vector<std::string>& options);
	~ChoiceModal() override;

	void MessageReceived(BMessage* msg) override;
	bool QuitRequested() override;

	// Show the window and block until a choice is made or it is closed.
	int Go();

private:
	// Release the wait semaphore exactly once, however the modal ends.
	void _Finish();

	sem_id fDoneSem;
	int    fResult   = -1;
	bool   fFinished = false;
};

// SessionItem — a BStringItem that remembers the .session file path so the
// sidebar can load or delete the file behind a selected row.
class SessionItem : public BStringItem {
public:
	SessionItem(const std::string& label, const std::string& path,
	            const std::string& title);
	const std::string& Path()  const { return fPath; }
	const std::string& Title() const { return fTitle; }
private:
	std::string fPath;
	std::string fTitle;   // raw title (no "  (N)" turn-count suffix)
};

// RenameModal — a tiny modal prompt with a single text field, used to
// rename a saved session. Go() returns the entered text, or empty on cancel.
class RenameModal : public BWindow {
public:
	explicit RenameModal(const std::string& current);
	~RenameModal() override;

	void Show() override;
	void MessageReceived(BMessage* msg) override;
	bool QuitRequested() override;

	// Show modally and return the entered name (empty = cancelled).
	std::string Go();

private:
	void _Finish();

	BTextControl* fField    = nullptr;
	sem_id        fDoneSem;
	std::string   fResult;
	bool          fFinished = false;
};

// SessionListView — BListView that pops a right-click context menu
// (Rename / Open / Delete) on the row under the cursor. The menu items post
// the shared sidebar messages to `target`, so the handlers are shared with
// the New/Open/Delete buttons.
class SessionListView : public BListView {
public:
	SessionListView(const char* name, BHandler* target);
	void MouseDown(BPoint where) override;
private:
	BHandler* fTarget = nullptr;
};

// NotifySlider — slider for the notification delay. Snaps to 10-second
// steps and shows a plain-English label ("Notify is disabled" / "Notify
// after 30s") that updates live as the thumb moves.
class NotifySlider : public BSlider {
public:
	NotifySlider(const char* name, const char* label, BMessage* message,
	             int32 minValue, int32 maxValue);

	// Attach a left-aligned label that mirrors the slider value.
	void SetValueLabel(BStringView* label);

	// Round every value change to the nearest 10 seconds.
	void SetValue(int32 value) override;

	// Suppress the slider's built-in right-aligned value text.
	const char* UpdateText() const override;

	// Render a notify delay (seconds) as a friendly phrase.
	static void FormatNotifyDelay(int32 seconds, char* out, size_t outSize);

private:
	void _UpdateLabel();
	BStringView* fLabel = nullptr;
};

#endif // HAIKU_CLAUDE_CLI_GUI_WIDGETS_H
