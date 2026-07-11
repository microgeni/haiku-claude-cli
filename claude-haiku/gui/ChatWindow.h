#ifndef CHAT_WINDOW_H
#define CHAT_WINDOW_H

#include <Window.h>
#include <String.h>

class BTextView;
class BTextControl;
class BButton;

// BMessage 'what' codes that cross the worker <-> main-thread boundary.
// API tokens and command output share these -- one rendering path for both.
enum {
    MSG_SEND        = 'send',  // input/button -> window: start a turn
    MSG_CHUNK       = 'chnk',  // worker -> window: streamed text (sink.onChunk)
    MSG_DONE        = 'done',  // worker -> window: turn complete (sink.onDone)
    MSG_ERR         = 'erro',  // worker -> window: failure (sink.onError)
    MSG_CANCEL      = 'cncl',  // user -> window: cancel in-flight turn
    MSG_CLEAR       = 'clr ',  // user -> window: clear the scrollback
    MSG_SETTINGS    = 'set ',  // user -> window: open the settings panel
};

// The GUI front-end. Owns all BViews on the main thread. A worker thread runs
// cch::AgentLoop::Turn with a StreamSink whose callbacks package text into the
// MSG_* BMessages above and SendMessage() them here. MessageReceived (main
// thread, under the window lock) does the actual BTextView::Insert -- the only
// thread-safe way to mutate the UI. This is what dissolves the old VTIME /
// polling responsiveness problem: the GUI is event-driven, tokens just arrive.
class ChatWindow : public BWindow {
public:
                    ChatWindow();
    virtual void    MessageReceived(BMessage* msg);
    virtual bool    QuitRequested();

private:
    void            _StartTurn();
    void            _Append(const char* text);
    void            _Clear();

    BTextView*      fOutput;   // scrollback, stylable (roles/code blocks)
    BTextControl*   fInput;
    BButton*        fSend;
    BButton*        fClear;
    BButton*        fSettings;
    thread_id       fWorker;   // in-flight turn, or -1
};

#endif
