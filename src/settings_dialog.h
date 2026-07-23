#ifndef HAIKU_CLAUDE_CLI_SETTINGS_DIALOG_H
#define HAIKU_CLAUDE_CLI_SETTINGS_DIALOG_H

#include <string>

#include <MenuField.h>
#include <Slider.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

// SettingsDialog — a free-floating dialog window for system prompt / model
// config. Contains the model picker, system-prompt editor, max-tokens field,
// a notification-delay slider, the working-directory field, and a Close
// button. Created once (hidden) and shown/hidden on demand; Browse and Close
// actions are forwarded to the parent window (via gui::MSG_BROWSE_WORKDIR /
// gui::MSG_SETTINGS) so the existing ChatWindow handlers stay shared.
//
// Extracted from chat_window so the dialog is a standalone translation unit.
// It couples to the owner only through a generic BWindow* parent and the
// shared gui::MSG_* codes — never a ChatWindow pointer.
class SettingsDialog : public BWindow {
public:
	SettingsDialog(BWindow* parent, const std::string& systemPrompt,
	               int maxTokens, int notifyMinSec,
	               const std::string& workingDir = {},
	               BMenuField* modelField = nullptr);

	// Populate fields from current config.
	void	SetValues(const std::string& systemPrompt, int maxTokens,
	                  int notifyMinSec, const std::string& workingDir = {});

	// Read back edited values.
	std::string	SystemPrompt() const;
	int         MaxTokens() const;
	bool        NotificationsEnabled() const;
	int         NotifyMinSeconds() const;
	std::string WorkingDir() const;

	bool	IsOpen() const { return fOpen; }
	void	Toggle();

	// Closing the dialog (X button) defers to the parent's MSG_SETTINGS
	// handler so edited values get read back, then hides instead of quitting.
	bool	QuitRequested() override;

private:
	void	_BuildLayout(const std::string& systemPrompt, int maxTokens,
	                     int notifyMinSec, const std::string& workingDir,
	                     BMenuField* modelField);

	BWindow*      fParent         = nullptr;
	BTextView*    fSysPromptView  = nullptr;
	BTextControl* fMaxTokensCtl   = nullptr;
	BSlider*      fNotifyDelay    = nullptr;
	BTextControl* fWorkingDirCtl  = nullptr;
	bool          fOpen           = false;
};

#endif // HAIKU_CLAUDE_CLI_SETTINGS_DIALOG_H
