#ifndef HAIKU_CLAUDE_CLI_GUI_COLORS_H
#define HAIKU_CLAUDE_CLI_GUI_COLORS_H

#include <InterfaceDefs.h>  // rgb_color

// gui_colors — the GUI's chat output palette, shared between chat_window and
// the extracted view classes (gui_views). The output area is intentionally
// always dark regardless of the system theme, so these are fixed rgb_color
// values rather than ui_color() lookups.
//
// Declared inline (C++17) at global scope so every translation unit that
// includes this header sees the same constants under their original
// unqualified names — no call-site changes were needed when they moved out
// of chat_window.cpp's anonymous namespace.

// Output area colours (always dark regardless of system theme).
inline const rgb_color kColorChatBg      = {  24,  24,  28, 255 };
inline const rgb_color kColorText        = { 215, 215, 220, 255 };
inline const rgb_color kColorUserLabel   = {  86, 180, 233, 255 };
inline const rgb_color kColorModelLabel  = { 204, 121,  90, 255 };
inline const rgb_color kColorToolLine    = { 130, 130, 140, 255 };
inline const rgb_color kColorError       = { 230,  75,  75, 255 };
inline const rgb_color kColorDiffAdd     = {  80, 200,  80, 255 }; // green  — added lines
inline const rgb_color kColorDiffRemove  = { 220,  80,  80, 255 }; // red    — removed lines
inline const rgb_color kColorDiffHeader  = { 140, 180, 220, 255 }; // steel-blue — diff header/meta
// Cyan used for the input text, echoing the CLI's cyan "you>" prompt.
// Bright cyan that pops on the dark input background (kColorChatBg).
inline const rgb_color kColorInputCyan   = {  60, 200, 215, 255 };

#endif // HAIKU_CLAUDE_CLI_GUI_COLORS_H
