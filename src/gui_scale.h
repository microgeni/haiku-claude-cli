#ifndef HAIKU_CLAUDE_CLI_GUI_SCALE_H
#define HAIKU_CLAUDE_CLI_GUI_SCALE_H

// gui_scale — HiDPI pixel scaling shared across the GUI front-end.
//
// Haiku does not expose a monitor DPI directly; instead the system font size
// grows on high-resolution displays (12pt is the 1x baseline). Deriving a
// scale factor from be_plain_font lets every hard-coded pixel dimension track
// the display so dialogs, buttons, bars, and icons stay proportional on HiDPI
// screens. Mirrors the be_control_look convention.
//
// Originally file-local helpers in chat_window.cpp; promoted to a shared TU so
// the extracted widget/dialog units can scale consistently.

namespace gui {

// Scale factor derived from the system plain font, clamped to [1.0, 4.0].
float Scale();

// Scale a 1x pixel measurement to the current display, rounded up so we never
// lose a pixel that would clip glyphs. (Named ScalePx, not Scale, because
// BView::Scale() already exists and would shadow a plain Scale().)
float ScalePx(float px);

} // namespace gui

#endif // HAIKU_CLAUDE_CLI_GUI_SCALE_H
