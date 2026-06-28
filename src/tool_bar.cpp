/*
 * Copyright 2025 Claude GUI for Haiku.
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Adapted from Genio's ToolBar (Copyright 2018 Kacper Kasper, MIT).
 * Border-drawing approach lifted from Tracker's Navigator.
 */


#include "tool_bar.h"

#include <Bitmap.h>
#include <Button.h>
#include <ControlLook.h>
#include <Handler.h>

#if defined(__HAIKU__)
#include <Application.h>
#include <IconUtils.h>
#include <Resources.h>
#endif


namespace gui {

namespace {

// Load a named B_VECTOR_ICON_TYPE resource into bitmap. Returns B_ERROR when
// the running binary has no such resource (the common case for the Claude
// GUI, which ships only the application icon). Mirrors Genio's GetVectorIcon.
status_t
load_vector_icon(const char* name, BBitmap* bitmap)
{
#if defined(__HAIKU__)
	if (bitmap == nullptr || name == nullptr || be_app == nullptr)
		return B_ERROR;

	BResources* resources = BApplication::AppResources();
	if (resources == nullptr)
		return B_ERROR;

	size_t size = 0;
	const uint8* rawIcon = reinterpret_cast<const uint8*>(
		resources->LoadResource(B_VECTOR_ICON_TYPE, name, &size));
	if (rawIcon == nullptr)
		return B_ERROR;

	return BIconUtils::GetVectorIcon(rawIcon, size, bitmap);
#else
	(void)name;
	(void)bitmap;
	return B_ERROR;
#endif
}

} // namespace


ToolBar::ToolBar(BHandler* defaultTarget, orientation orient,
	border_position border)
	:
	BToolBar(orient),
	fDefaultTarget(defaultTarget),
	fIconSize(24.0f),
	fBorder(border)
{
	// A 1px inset on the bordered edge leaves room for the border stroke,
	// the same trick BMenuBar uses (see Genio's ToolBar).
	if (fBorder == B_BORDER_BOTTOM)
		GroupLayout()->SetInsets(0.0f, 0.0f, 0.0f, 1.0f);
	else if (fBorder == B_BORDER_TOP)
		GroupLayout()->SetInsets(0.0f, 1.0f, 0.0f, 0.0f);

	// B_WILL_DRAW so Draw() runs; clear B_PULSE_NEEDED to avoid a BToolBar
	// tool-tip-hide bug noted in Genio.
	SetFlags((Flags() | B_WILL_DRAW) & ~B_PULSE_NEEDED);
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}


void
ToolBar::Draw(BRect updateRect)
{
	if (fBorder == B_BORDER_NONE) {
		BToolBar::Draw(updateRect);
		return;
	}

	BRect rect(Bounds());
	rgb_color base = LowColor();
	uint32 flags = 0;
	uint32 borders = (fBorder == B_BORDER_TOP)
		? BControlLook::B_TOP_BORDER
		: BControlLook::B_BOTTOM_BORDER;

	be_control_look->DrawBorder(this, rect, updateRect, base,
		B_PLAIN_BORDER, flags, borders);

	BToolBar::Draw(rect & updateRect);
}


void
ToolBar::AddAction(uint32 command, const char* toolTipText,
	const char* iconName, bool lockable)
{
	BBitmap* icon = nullptr;
	if (iconName != nullptr) {
		icon = new BBitmap(BRect(0, 0, fIconSize - 1.0f, fIconSize - 1.0f),
			0, B_RGBA32);
		if (load_vector_icon(iconName, icon) == B_OK)
			fActionIcons.emplace(command, iconName);
		else {
			delete icon;
			icon = nullptr;
		}
	}
	BToolBar::AddAction(command, fDefaultTarget, icon, toolTipText,
		nullptr, lockable);
	delete icon;
}


void
ToolBar::AddAction(BMessage* msg, const char* toolTipText,
	const char* iconName, bool lockable)
{
	BBitmap* icon = nullptr;
	if (iconName != nullptr) {
		icon = new BBitmap(BRect(0, 0, fIconSize - 1.0f, fIconSize - 1.0f),
			0, B_RGBA32);
		if (load_vector_icon(iconName, icon) == B_OK)
			fActionIcons.emplace(msg->what, iconName);
		else {
			delete icon;
			icon = nullptr;
		}
	}
	BToolBar::AddAction(msg, fDefaultTarget, icon, toolTipText,
		nullptr, lockable);
	delete icon;
}


BButton*
ToolBar::AddLabelAction(uint32 command, const char* label)
{
	// BToolBar::AddAction with a text and no icon yields a labelled button.
	BToolBar::AddAction(command, fDefaultTarget, nullptr, nullptr, label,
		false);
	return BToolBar::FindButton(command);
}


BButton*
ToolBar::FindButton(uint32 command) const
{
	return BToolBar::FindButton(command);
}


void
ToolBar::ChangeIconSize(float newSize)
{
	fIconSize = newSize;
	for (const auto& action : fActionIcons) {
		BBitmap icon(BRect(0, 0, fIconSize - 1.0f, fIconSize - 1.0f),
			0, B_RGBA32);
		if (load_vector_icon(action.second.c_str(), &icon) != B_OK)
			continue;
		BButton* button = BToolBar::FindButton(action.first);
		if (button != nullptr)
			button->SetIcon(&icon);
	}
}


void
ToolBar::SetActionEnabled(uint32 command, bool enable)
{
	BToolBar::SetActionEnabled(command, enable);
}


void
ToolBar::SetEnabled(bool enable)
{
	for (int32 i = 0; i < GroupLayout()->CountItems(); i++) {
		BControl* control =
			dynamic_cast<BControl*>(GroupLayout()->ItemAt(i)->View());
		if (control != nullptr)
			control->SetEnabled(enable);
	}
}


void
ToolBar::SetTarget(BHandler* defaultTarget)
{
	fDefaultTarget = defaultTarget;
	for (int32 i = 0; i < GroupLayout()->CountItems(); i++) {
		BControl* control =
			dynamic_cast<BControl*>(GroupLayout()->ItemAt(i)->View());
		if (control != nullptr)
			control->SetTarget(fDefaultTarget);
	}
}


void
ToolBar::ToggleActionPressed(uint32 command)
{
	for (int32 i = 0; BView* view = BToolBar::ChildAt(i); i++) {
		BButton* button = dynamic_cast<BButton*>(view);
		if (button == nullptr)
			continue;
		BMessage* message = button->Message();
		if (message == nullptr)
			continue;
		button->SetValue(message->what == command);
	}
}

} // namespace gui
