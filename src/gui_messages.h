#ifndef HAIKU_CLAUDE_CLI_GUI_MESSAGES_H
#define HAIKU_CLAUDE_CLI_GUI_MESSAGES_H

#include <cstdint>

// gui_messages — BMessage `what` codes shared across the GUI front-end.
//
// These were originally defined in chat_window.h, but they form a shared
// vocabulary used by ChatWindow, the helper widgets (gui_widgets), the app
// delegate (app_main_gui), and the tool bar — not ChatWindow internals.
// Keeping them in a small standalone header lets those translation units
// depend on the message IDs without pulling in the whole ChatWindow
// declaration. (Codes in gui_sink.h are separate and stay there.)

namespace gui {

constexpr uint32_t MSG_NEW_CHAT     = 'NCVT'; // clear conversation + display
constexpr uint32_t MSG_CLEAR_OUTPUT = 'CLRO'; // clear display only (keep ctx)
constexpr uint32_t MSG_CANCEL       = 'CNCL'; // cancel in-flight turn
constexpr uint32_t MSG_MODEL_PICK   = 'MPCK'; // model menu item selected
constexpr uint32_t MSG_SETTINGS     = 'STNG'; // toggle settings panel
constexpr uint32_t MSG_TOKENS       = 'TOKN'; // int32 "input","output","max"
constexpr uint32_t MSG_JUMP_BOTTOM  = 'JBOT'; // jump-to-bottom button
constexpr uint32_t MSG_TICK         = 'TICK'; // 80-ms spinner tick
// MSG_SESSIONS / MSG_SESSION_LOAD reserved for future project.
constexpr uint32_t MSG_MODELS_READY = 'MDLS'; // background model fetch complete
constexpr uint32_t MSG_ABOUT        = 'ABUT'; // Help > About Claude
constexpr uint32_t MSG_HELP_DOCS    = 'HDOC'; // Help > Documentation
constexpr uint32_t MSG_DEMO_MARKDOWN = 'DMMD'; // Help > Show Markdown Demo
constexpr uint32_t MSG_LUDICROUS     = 'LUDC'; // Tools > Ludicrous Mode toggle
constexpr uint32_t MSG_REMOTE_CONTROL = 'RMTC'; // Tools > Remote Control toggle
constexpr uint32_t MSG_REMOTE_APPEND  = 'RMAP'; // remote turn finished: append to history
constexpr uint32_t MSG_BROWSE_WORKDIR = 'BRWD'; // Settings: browse for working dir
constexpr uint32_t MSG_EXPORT         = 'EXPT'; // File > Export Transcript…
constexpr uint32_t MSG_EXPORT_SAVE    = 'EXPS'; // B_SAVE_REQUESTED from export panel
constexpr uint32_t MSG_FIND           = 'FIND'; // Cmd-F: toggle the find bar
constexpr uint32_t MSG_FIND_NEXT      = 'FNXT'; // find bar: next match / Enter
constexpr uint32_t MSG_FIND_PREV      = 'FPRV'; // find bar: previous match
constexpr uint32_t MSG_FIND_CLOSE     = 'FCLO'; // find bar: close / Esc
constexpr uint32_t MSG_FIND_LIVE      = 'FLIV'; // find bar: query text changed
constexpr uint32_t MSG_ZOOM_IN        = 'ZMIN'; // Cmd-+ : larger chat font
constexpr uint32_t MSG_ZOOM_OUT       = 'ZMOT'; // Cmd-- : smaller chat font
constexpr uint32_t MSG_ZOOM_RESET     = 'ZMRS'; // Cmd-0 : reset chat font
constexpr uint32_t MSG_TOGGLE_SESSIONS = 'TSES'; // View: toggle session sidebar
constexpr uint32_t MSG_TOGGLE_INPUT    = 'TINP'; // View: toggle the input bar
constexpr uint32_t MSG_SESSION_SELECT  = 'SSEL'; // sidebar: load selected session
constexpr uint32_t MSG_SESSION_DELETE  = 'SDEL'; // sidebar: delete selected session
constexpr uint32_t MSG_SESSION_NEW     = 'SNEW'; // sidebar: start a new chat
constexpr uint32_t MSG_SESSION_RENAME  = 'SRNM'; // sidebar: rename selected session
constexpr uint32_t MSG_NEW_WINDOW      = 'NWIN'; // File > New Session: spawn a window
constexpr uint32_t MSG_COMPACT         = 'CMPT'; // Edit: compact conversation context
constexpr uint32_t MSG_CLEAR_HISTORY   = 'CLRH'; // Edit: clear saved prompt history

} // namespace gui

#endif // HAIKU_CLAUDE_CLI_GUI_MESSAGES_H
