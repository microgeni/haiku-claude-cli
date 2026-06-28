/*
 * Copyright 2025 Claude GUI for Haiku.
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Adapted from Genio's ToolBar (Copyright 2018 Kacper Kasper, MIT),
 * which is the golden-standard toolbar pattern for Haiku applications:
 * a BPrivate::BToolBar subclass that draws a 1px border (like BMenuBar),
 * keeps a registry of its actions, and can retarget/enable them en masse.
 *
 * This variant additionally supports text-label buttons, because the
 * Claude GUI ships a single application icon (HVIF) rather than a resource
 * file full of named vector icons. AddAction() with no icon name falls
 * back to a plain labelled BButton placed in the toolbar's group layout.
 */
#ifndef HAIKU_CLAUDE_CLI_TOOL_BAR_H
#define HAIKU_CLAUDE_CLI_TOOL_BAR_H


#include <Control.h>
#include <ToolBar.h>

#include <string>
#include <unordered_map>


class BButton;
class BHandler;


namespace gui {

// ToolBar — a BToolBar with a bottom (or top) border and a label-button
// fallback. Used for the chat window's bottom input pane action row and any
// other compact button strips.
class ToolBar : public BPrivate::BToolBar {
public:
	enum border_position {
		B_BORDER_BOTTOM,
		B_BORDER_TOP,
		B_BORDER_NONE,
	};

							ToolBar(BHandler* defaultTarget = nullptr,
								orientation orient = B_HORIZONTAL,
								border_position border = B_BORDER_BOTTOM);

	virtual void			Draw(BRect updateRect) override;

	// Icon action (loads a named B_VECTOR_ICON_TYPE resource). On builds
	// without a resource file this draws an empty button; prefer the label
	// overload for the Claude GUI.
	void					AddAction(uint32 command, const char* toolTipText,
								const char* iconName = nullptr,
								bool lockable = false);

	void					AddAction(BMessage* msg, const char* toolTipText,
								const char* iconName = nullptr,
								bool lockable = false);

	// Label action — adds a normal BButton carrying `label` that posts
	// `command` to the default target. Returns the button so callers can
	// keep a handle (e.g. to MakeDefault, show/hide, or relabel it).
	BButton*				AddLabelAction(uint32 command, const char* label);

	// Find a button previously added by command id (icon or label).
	BButton*				FindButton(uint32 command) const;

	void					ChangeIconSize(float newSize);
	void					SetActionEnabled(uint32 command, bool enable);
	void					SetEnabled(bool enable);
	void					SetTarget(BHandler* defaultTarget);
	void					ToggleActionPressed(uint32 command);

private:
	BHandler*				fDefaultTarget;
	float					fIconSize;
	border_position			fBorder;
	// command -> icon resource name, for ChangeIconSize().
	std::unordered_map<uint32, std::string>	fActionIcons;
};

} // namespace gui


#endif // HAIKU_CLAUDE_CLI_TOOL_BAR_H
