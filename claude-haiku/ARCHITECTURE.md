# Architecture: adding a GUI front-end to haiku-claude-cli

This supersedes the greenfield sketch. The CLI already exists at v1.8.1 with a
working agentic core (libcurl + OpenSSL + nlohmann/json + libedit, single C++17
binary). The GUI is **not a new client** -- it is a second front-end over the
existing core, attached through one new interface. The honest framing of the
whole effort is: *carve a UI seam through the existing src/, then add a BeAPI
front-end as a third consumer alongside the terminal REPL and the Telegram
bridge.*

## What already exists (the de-facto core)

From src/, these modules are logic and become the shared core unchanged:
api, tools, config, oauth, models, mcp, hooks, paths. Notably:

- `api::SendConversation` / `api::SendWithTools` -- streamed POST + tool-use
  loop. Pure functions over JSON.
- `tools::Definitions/Run/RequiresPermission/Preview` -- the cleanest interface
  in the tree; returns a struct, no UI types.
- BFS integration is ALREADY in the core and ALREADY Haiku-native: the
  `claude:summary` attribute cache, `api::DrainWrittenSummaryPaths`, the
  Query()-backed startup context. The GUI inherits all of this for free.

TLS was never a risk: libcurl + OpenSSL, BeAPI-free, drops straight in.

## The seam: OutputSink

The one real refactor. Today api.cpp/tools.cpp reach for the terminal directly
(57 + 13 `tui::` calls) AND already fan out to the Telegram bridge through
ad-hoc extern globals (`g_stream_progress`, `g_tool_status_hook`,
`g_telegram_permission_hook`). That fan-out is StreamSink in embryo -- the seam
was already cut once, informally, when Telegram was built.

Promote those into one explicit interface. The SSE text_delta site
(api.cpp ~309-318) today does:

    state->renderer.Write(chunk);            // terminal, inlined
    if (g_stream_progress) { ...append... }  // telegram, inlined

becomes:

    sink.OnText(chunk);                      // one call, N implementations

### OutputSink interface (~5 methods, derived from existing g_* hook types)

    struct OutputSink {
        virtual void OnText(const std::string& chunk) = 0;          // streamed assistant text
        virtual void OnToolStatus(const std::string& phase) = 0;    // "running Bash...", cleared on done
        virtual Permission AskPermission(const std::string& tool,
                                         const std::string& preview) = 0;
        virtual void OnStatus(const std::string& s) = 0;            // spinner / meta notices
        virtual void OnError(const std::string& s) = 0;
    };

Scroll-region / cursor-positioning calls (SuspendScrollRegion,
PositionCursorForChat, ...) do NOT enter the interface -- they are terminal
plumbing and become no-ops in non-terminal sinks. The pure string formatters
(Dim, Bold, HighlightCode, DiffAdded...) are not part of the sink either; they
stay as free formatting helpers the terminal sink uses.

### Two sink families, not one flat interface

The five-method OutputSink above is the minimum the core must emit. But the
front-ends fall into two presentation models that are genuinely different, and
forcing them into one flat sink is what makes Telegram bad today. See the next
section -- the sink interface splits into StreamSink (terminal) and
StructuredSink (GUI + Telegram).

Refactor scope: thread a sink reference through api.cpp/tools.cpp; the core
emits semantic events, not terminal writes. TerminalSink wraps the existing
tui:: calls (CLI behaves identically; tui/repl/session ~120 KB unchanged).
StructuredSink is implemented twice (GUI, Telegram). Not a rewrite.

## Front-end presentation models: stream vs structured

The CLI lives in a STREAM: one append-only scrollback, everything is text
flowing top to bottom, presence (prompts, status, spinner) woven into the
stream via cursor tricks and scroll regions. Native to a terminal, alien
elsewhere.

The GUI and Telegram are MESSAGE-AND-WIDGET surfaces. A reply is a discrete
unit. A permission request is a separate element with buttons, not inline text
you type `y` after. Status is a transient indicator, not a transcript line.
Tool output is collapsible, not dumped. They think in messages-with-affordances,
not a character stream.

This is WHY Telegram feels bad now: the bridge adapted the CLI's stream model
onto a chat surface through ad-hoc hooks. "Too much text, prompts in the wrong
place" is the stream model leaking through -- terminal-shaped output (meta
notices, status lines, full-transcript mirroring) flattened into chat messages.
In a scrollback, showing everything is free; in chat, every line is a
notification, so the same volume is noise.

### The dividing line: share events + logic, NOT rendering

A phone has a few square inches and a thumb; a desktop window has a thousandfold
the space and a pointer. Any abstraction that shares LAYOUT is wrong on both.
What GUI and Telegram share is upstream of pixels:

- the EVENT MODEL -- what happened (message began, tool started, permission
  needed, choice offered, message ended).
- the DECISION LOGIC -- what is worth showing and what to suppress; how a
  permission "first responder wins" across surfaces; session/history semantics.

What they do NOT share: how any of it looks. Each surface owns its own
rendering against its own constraints.

### StreamSink (terminal)

OnText appends; status uses cursor control; permission reads a keypress. The
current tui:: model, wrapped. Power-user detail (token counts, turn timings,
cache stats) is fine here -- in a scrollback it is free.

### StructuredSink (GUI + Telegram)

Semantic, affordance-oriented events -- a first cut:

    BeginMessage(role)                         // start a discrete reply unit
    AppendText(chunk)                          // streamed text into current msg
    EndMessage()                               // reply complete -> finalize unit
    ToolStarted(name, summary)                 // collapsible; not full dump
    ToolFinished(name, ok, detail)             // detail hidden by default
    AskChoice(prompt, options[]) -> index      // buttons, NOT "type 1-4"
    AskPermission(tool, preview) -> Permission  // y/a/n as affordances
    SetStatus(kind)                            // transient indicator, not text
    OnError(message)

Both implement this; each renders differently:

- **GUI (GuiSink)** -- BeginMessage/EndMessage map to message bubbles or
  styled BTextView sections; AskChoice/AskPermission to native dialogs or
  buttons (NOT SelectOption -- the VTIME bug stops existing); SetStatus to a
  spinner widget; tool detail to a collapsible region; CodeStyler renders code.
  No rate limits, wants smooth token streaming.
- **Telegram (TelegramSink)** -- BeginMessage posts a placeholder, AppendText
  batches edits (rate-limit aware), EndMessage settles the final text;
  AskChoice/AskPermission to inline keyboards; SetStatus to the typing
  indicator; tool detail omitted or one-line. Must batch to dodge edit limits.

Same SEMANTICS, independent RENDERING. The model says WHAT and WHAT-TO-SHOW;
each implementation owns HOW.

### Lean Telegram: what it should STOP showing

Drawn from the current pain points -- the StructuredSink model makes these
decisions once, centrally, instead of leaking terminal chrome:

- NO full-transcript local mirror -- the phone is a presence mode, not a second
  copy of the desktop session's scrollback.
- NO meta notices / status lines as chat messages -- status is the typing
  indicator, ephemeral, never a sent message.
- NO token/timing/cache stats by default -- power-user data belongs to the
  terminal; surface on explicit /usage only.
- NO scroll-region / cursor artifacts -- terminal-only, never reach a sink event.
- Tool calls COLLAPSED -- "ran Bash ✓", expandable on request, not full output
  inline.
- One reply = one settled message -- not a sequence of edit-spam fragments left
  behind in the chat.

The fix for Telegram is therefore a CONSEQUENCE of defining StructuredSink
properly, not a separate redo: its current problems are all "stream model forced
into a chat surface." Build the structured model for the GUI, implement Telegram
against the same model, and the leanness falls out.

## GUI front-end (BeAPI)

Replaces tui+repl+session with: BApplication + ChatWindow (BWindow). Worker
thread runs api::SendWithTools with a GuiSink; the sink's callbacks marshal to
the window via BMessenger. MessageReceived (main thread, window lock) does the
actual view mutation -- the only thread-safe way to touch a BView.

### Rendering: three surfaces, each where it is strongest

The terminal flattens everything into ANSI. The GUI splits by content type:

1. **Prose markdown -> BTextView styled runs.** Real heading sizes, monospace
   inline code, indented blockquotes, bullet/numbered lists, clickable links
   (B_NAVIGABLE). Far better than the ANSI markdown renderer.
2. **Fenced code blocks -> embedded Scintilla, themed (see below).**
3. **Web content (WebFetch) -> formatted BTextView, or optionally BWebView
   (HaikuWebKit) for true HTML rendering.**

## CodeStyler: Genio-themed syntax highlighting

The standout "visibly yours" feature: code blocks render in the SAME theme as
the user's Genio editor.

Both Genio and Koder are Scintilla-for-Haiku editors; the reusable asset is the
**Scintilla + Lexilla engine** (pkgman: lexilla_devel), NOT either app's code.
Embed the engine directly and feed it Genio's YAML theme + language files.
Theme files are MIT-licensed (Konradsson) -- license-clean to read and bundle.
The user's custom Genio theme is already in this exact format.

### Schema (confirmed against data/styles/dark.yaml + data/languages/c.yaml)

- THEME `styles/<name>.yaml`: named style -> { id, foreground, background,
  style[] }. `id` is Genio's CANONICAL style number (Keyword=105, String=106,
  Comment=101, ...). A `Global:` block maps to reserved Scintilla styles
  (Default=32 -> STYLE_DEFAULT, Line number=33 -> STYLE_LINENUMBER, Brace
  highlight=34 -> STYLE_BRACELIGHT).
- LANGUAGE `languages/<name>.yaml`: `lexer:` (Lexilla name), `keywords:` (per
  group), and `styles:` mapping <scintilla_style_num> -> <canonical_id>. That
  indirection is how ONE theme colors every language.

### Apply loop

    SCI_SETILEXER(language.lexer)
    for each keyword group g: SCI_SETKEYWORDS(g, language.keywords[g])
    for each (sciStyleNum -> canonId) in language.styles:
        t = theme.ByCanonicalId(canonId)
        SCI_STYLESETFORE(sciStyleNum, hex(t.foreground))
        if t.background: SCI_STYLESETBACK(sciStyleNum, hex(t.background))
        if t.bold/italic/underline: SCI_STYLESET{BOLD,ITALIC,UNDERLINE}(...)
    ApplyGlobal()   // STYLE_DEFAULT, STYLE_LINENUMBER, caret, selection...

Header: gui/styling/CodeStyler.h. Reference schema files (MIT):
gui/styling/reference/genio-{dark,lang-c,lang-markdown}.yaml.

### Streaming nuance

Scintilla wants a buffer to lex, but Claude streams code token by token. Two
phase: render the block as plain monospace text while tokens arrive, then once
the closing fence is seen (and the language known) instantiate/lex a themed
Scintilla view. Matches the "buffer for processing, stream for display" pattern.

### Cost note

A full Scintilla instance per block has real overhead. For many small blocks,
prefer styled BTextView runs and reserve embedded Scintilla for larger blocks,
or use one reusable Scintilla view in a code pane. Decide per-block by size.

## Build targets

- libclaudecore.a : api, tools, config, oauth, models, mcp, hooks, paths
  + the OutputSink interface. BeAPI-free.
- claude-cli      : libcore + TerminalSink + tui/repl/session/commands/stats
  + TelegramSink. Links libcurl/openssl/libedit. Unchanged behavior.
- claude-gui      : libcore + GuiSink + ChatWindow + CodeStyler. Links libcore,
  libbe, libcurl/openssl, Scintilla/Lexilla, yaml-cpp.

## Sequenced work

1. Extract libclaudecore.a: move the logic modules behind the build seam; no
   behavior change. Verify the CLI still builds/links/runs.
2. Introduce the sink seam in api.cpp/tools.cpp so the core emits semantic
   events. Implement StreamSink (TerminalSink) by lifting existing tui:: code.
   CLI identical afterward. (The substantive decoupling step.)
3. Define StructuredSink (the message+affordance event model + the lean
   "what to suppress" rules). This is the shared semantic layer for GUI +
   Telegram -- design it once, here.
4. GUI shell: BApplication + ChatWindow + GuiSink (a StructuredSink impl) +
   worker threading. Plain BTextView rendering first (no code styling yet).
5. Re-implement Telegram as a second StructuredSink against the model from (3),
   replacing the current stream-model bridge. Leanness falls out of the model.
6. CodeStyler: yaml-cpp parsers for theme + language files; embed Scintilla;
   apply loop; two-phase streaming swap.
7. Markdown polish + optional BWebView for WebFetch.

Note: steps 4 and 5 share the StructuredSink contract from step 3 but share NO
rendering code -- the GUI renders for a desktop window, Telegram for a phone
chat. Shared model, independent surfaces.
