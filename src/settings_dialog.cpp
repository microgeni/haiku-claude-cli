#include "settings_dialog.h"

#include <cstdlib>
#include <string>

#include <Box.h>
#include <Button.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <StringView.h>

#include "gui_messages.h"
#include "gui_scale.h"
#include "gui_widgets.h"

using gui::ScalePx;

SettingsDialog::SettingsDialog(BWindow* parent, const std::string& systemPrompt,
                               int maxTokens, int notifyMinSec,
                               const std::string& workingDir,
                               BMenuField* modelField)
	: BWindow(BRect(0, 0, ScalePx(640), ScalePx(480)), "Settings",
	          B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
	          B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	  fParent(parent)
{
	_BuildLayout(systemPrompt, maxTokens, notifyMinSec, workingDir, modelField);
	// Give the window a sensible default size: wide enough to read a full
	// working-directory path, then center it. AUTO_UPDATE_SIZE_LIMITS only
	// sets the minimum, so resize explicitly to the preferred width.
	// Scaled so the dialog grows with the system font on HiDPI displays.
	ResizeTo(ScalePx(640), ScalePx(480));
	CenterOnScreen();
	// Start the looper running but keep the window off-screen: Hide() before
	// Show() leaves a net-hidden window whose looper is alive, so Toggle()
	// can reveal it instantly and the parent can lock it to read values.
	Hide();
	Show();
}

void SettingsDialog::_BuildLayout(const std::string& systemPrompt, int maxTokens,
                                  int notifyMinSec, const std::string& workingDir,
                                  BMenuField* modelField)
{
	// System-prompt label + editor.
	BStringView* sysLabel = new BStringView("syslbl", "System Prompt:");
	fSysPromptView        = new BTextView("sysprompt", B_WILL_DRAW | B_FRAME_EVENTS);
	fSysPromptView->SetWordWrap(true);
	fSysPromptView->SetText(systemPrompt.c_str());
	fSysPromptView->SetExplicitMinSize(BSize(ScalePx(80), ScalePx(80)));
	BScrollView* sysScroll = new BScrollView("sysscroll", fSysPromptView,
	                                          0, false, true, B_FANCY_BORDER);
	sysScroll->SetExplicitMinSize(BSize(ScalePx(100), ScalePx(80)));
	sysScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

	// Max tokens field.
	fMaxTokensCtl = new BTextControl("maxtokens", "Max tokens:",
	                                  std::to_string(maxTokens).c_str(), nullptr);

	// Notification delay slider: how long a turn must run before it
	// fires a desktop notification. 0 = notifications disabled. A
	// left-aligned label mirrors the value in plain English.
	BStringView* notifyLabel = new BStringView("notifylbl", "");
	notifyLabel->SetAlignment(B_ALIGN_LEFT);
	NotifySlider* notifySlider = new NotifySlider("notifydelay", nullptr,
	                                              nullptr, 0, 300);
	fNotifyDelay = notifySlider;
	fNotifyDelay->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fNotifyDelay->SetHashMarkCount(7);
	fNotifyDelay->SetKeyIncrementValue(10);
	if (notifyMinSec < 0)   notifyMinSec = 0;
	if (notifyMinSec > 300) notifyMinSec = 300;
	fNotifyDelay->SetValue(notifyMinSec);
	notifySlider->SetValueLabel(notifyLabel);

	// Working directory field + Browse button.
	// The text control holds the path; the Browse button opens a
	// BFilePanel so the user can pick a directory without typing.
	fWorkingDirCtl = new BTextControl("workingdir", "Working directory:",
	                                   workingDir.c_str(), nullptr);
	fWorkingDirCtl->SetToolTip("Directory Claude uses as the root for "
	                            "relative file paths and tool calls.");
	// Make the path field roomy enough to read a full path without scrolling.
	fWorkingDirCtl->SetExplicitMinSize(BSize(ScalePx(360), B_SIZE_UNSET));
	BButton* browseBtn = new BButton("browseworkdir", "Browse" B_UTF8_ELLIPSIS,
	                                  new BMessage(gui::MSG_BROWSE_WORKDIR));
	BButton* closeBtn = new BButton("closesettings", "Close",
	                                 new BMessage(gui::MSG_SETTINGS));
	closeBtn->MakeDefault(true);

	// Browse and Close are handled by the parent ChatWindow (the working-dir
	// BFilePanel and the value read-back logic both live there).
	if (fParent) {
		browseBtn->SetTarget(fParent);
		closeBtn->SetTarget(fParent);
	}

	// Group the controls into labelled BBoxes, mirroring Genio's ConfigWindow
	// idiom (MakeViewFor wraps each settings group in a BBox with a label).
	BBox* modelBox = new BBox("modelbox");
	modelBox->SetLabel("Model");
	BGroupView* modelGroup = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	modelGroup->GroupLayout()->SetInsets(B_USE_ITEM_INSETS);
	BLayoutBuilder::Group<>(modelGroup)
		.Add(modelField)
		.Add(fMaxTokensCtl)
		.Add(sysLabel)
		.Add(sysScroll, 1.0f)
	.End();
	modelBox->AddChild(modelGroup);

	BBox* behaviorBox = new BBox("behaviorbox");
	behaviorBox->SetLabel("Behavior");
	BGroupView* behaviorGroup = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	behaviorGroup->GroupLayout()->SetInsets(B_USE_ITEM_INSETS);
	BLayoutBuilder::Group<>(behaviorGroup)
		.Add(notifyLabel)
		.Add(fNotifyDelay)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(fWorkingDirCtl, 1.0f)
			.Add(browseBtn, 0.0f)
		.End()
	.End();
	behaviorBox->AddChild(behaviorGroup);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(modelBox, 1.0f)
		.Add(behaviorBox, 0.0f)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.AddGlue()
			.Add(closeBtn)
		.End()
	.End();
}

void SettingsDialog::SetValues(const std::string& systemPrompt, int maxTokens,
                               int notifyMinSec, const std::string& workingDir)
{
	if (fSysPromptView)
		fSysPromptView->SetText(systemPrompt.c_str());
	if (fMaxTokensCtl)
		fMaxTokensCtl->SetText(std::to_string(maxTokens).c_str());
	if (fNotifyDelay) {
		if (notifyMinSec < 0)   notifyMinSec = 0;
		if (notifyMinSec > 300) notifyMinSec = 300;
		fNotifyDelay->SetValue(notifyMinSec);
	}
	if (fWorkingDirCtl)
		fWorkingDirCtl->SetText(workingDir.c_str());
}

std::string SettingsDialog::SystemPrompt() const
{
	if (!fSysPromptView) return {};
	const char* t = fSysPromptView->Text();
	return t ? std::string(t) : std::string();
}

int SettingsDialog::MaxTokens() const
{
	if (!fMaxTokensCtl) return 8192;
	const char* t = fMaxTokensCtl->Text();
	if (!t || t[0] == '\0') return 8192;
	int v = std::atoi(t);
	return (v > 0) ? v : 8192;
}

bool SettingsDialog::NotificationsEnabled() const
{
	// A delay of 0 means notifications are turned off entirely.
	if (!fNotifyDelay) return true;
	return fNotifyDelay->Value() > 0;
}

int SettingsDialog::NotifyMinSeconds() const
{
	if (!fNotifyDelay) return 5;
	return static_cast<int>(fNotifyDelay->Value());
}

std::string SettingsDialog::WorkingDir() const
{
	if (!fWorkingDirCtl) return {};
	const char* t = fWorkingDirCtl->Text();
	return t ? std::string(t) : std::string();
}

void SettingsDialog::Toggle()
{
	fOpen = !fOpen;
	if (fOpen) {
		if (IsHidden())
			Show();
		Activate(true);
	} else {
		if (!IsHidden())
			Hide();
	}
}

bool SettingsDialog::QuitRequested()
{
	// The window is created once and kept alive (hidden) for the life of
	// the app, so a close request from the title-bar X must not actually
	// quit it. Defer to the parent's MSG_SETTINGS handler (which reads the
	// edited values back, then calls Toggle() to hide us).
	if (fOpen && fParent)
		fParent->PostMessage(gui::MSG_SETTINGS);
	return false;
}
