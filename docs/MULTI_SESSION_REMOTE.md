# Multi-Session Remote Control — Design

Status: **Proposal / not yet implemented**
Audience: maintainers
Related: `src/telegram.{cpp,h}`, `src/chat_window.{cpp,h}`,
`src/app_main_gui.cpp`, `src/session.cpp`

## Goal

Run several Claude sessions on the desktop and let a **Telegram user
switch between them from their phone** — list sessions, attach to one,
send prompts to it, and switch away. The phone is the *primary* driver
of session switching; the desktop is where sessions are spawned. The
desktop may also switch which session is "focused" locally, but that is
secondary.

## The one hard constraint

Telegram allows **exactly one `getUpdates` long-poll consumer per bot
token**. A second poller receives HTTP 409 "Conflict: terminated by
other getUpdates request" and the live poller silently stops receiving
updates. (See `Client::CheckPollAvailable` and the 409 handling in
`Client::poll`.)

**Consequence:** there can be only **one poller** for the whole bot,
regardless of how many sessions exist. Multiple sessions must therefore
*share a single poller* and route ("demultiplex") incoming updates to
the right session in software. Any design with one poller per session is
ruled out — it would 409 itself.

## Where we are today

```
app_main_gui.cpp ReadyToRun()
    └── new ChatWindow(...)               // exactly ONE window
            └── fRemote (unique_ptr<RemoteControl>)   // per-window poller
                    └── Client            // owns ONE bot token + ONE offset
```

- One process creates **one** `ChatWindow`.
- Each `ChatWindow` owns its **own** `RemoteControl`, which owns its own
  poller thread, `Client`, and `getUpdates` offset.
- `RemoteControl` hard-binds to a single conversation: `fUserMessages`
  (keyed by Telegram `user_id`), one `SetSharedHistory` provider, one
  `SetSharedHistoryAppender`, one global turn lock, one `fActiveChatId`.
- The CLI mirrors this: one `remote` in `session.cpp` bound to the REPL's
  `messages[]`.

So "session" at runtime == "the single conversation the one poller is
wired to." There is no registry, no IDs, no routing, and no multi-window
support. (`session_store.cpp`'s "sessions" are saved *files* — data at
rest — not live, pollable conversations.)

## Target architecture

Split the monolithic `RemoteControl` into two roles:

```
            ┌─────────────────────────────────────────────┐
            │  Dispatcher (process-wide, ONE per bot)      │
            │   • owns the single Client + PollLoop        │
            │   • owns the getUpdates offset               │
            │   • routes each Update to a Session by the   │
            │     per-chat "active session" pointer        │
            │   • owns session registry (id -> Session*)   │
            └───────────────┬──────────────┬───────────────┘
                            │              │
                  ┌─────────▼───┐    ┌─────▼─────────┐
                  │ Session #1  │    │ Session #2    │  ...
                  │ • fMessages │    │ • fMessages   │
                  │ • history   │    │ • history     │
                  │   provider  │    │   appender    │
                  │ • turn lock │    │ • turn lock   │
                  └─────────────┘    └───────────────┘
```

- **Dispatcher** — the only thing that touches Telegram `getUpdates`.
  One per process (later: optionally one per machine, see "Open
  questions"). Holds the session registry and a per-chat map
  `chat_id -> active session id`.
- **Session** — what `RemoteControl` is today *minus* the poller: its
  own context (`fUserMessages`), `SharedHistory` provider/appender,
  turn lock, and the GUI/CLI surface it's bound to. Carries a small
  **unique integer ID** assigned at registration.

### Routing model

Each Telegram chat has an **active session pointer**. An incoming update
is routed to whatever session that chat is currently attached to.
Built-in commands handled by the Dispatcher *before* routing:

| Command          | Effect                                                    |
|------------------|-----------------------------------------------------------|
| `/sessions`      | List sessions: `#id  title  (model)  [active]`            |
| `/session N`     | Attach this chat to session `N`; replies now go there     |
| `/whoami`        | Show which session this chat is currently attached to     |
| (existing) `/new`, `/mute`, `/unmute`, … | Apply to the *active* session     |

Default active session = the most recently created (or a configurable
"default"). If a chat has no active session yet, the Dispatcher replies
with a short `/sessions` hint instead of silently dropping the prompt.

## Required changes (touch-list)

1. **`telegram.h/.cpp` — split `RemoteControl`.**
   - New `Dispatcher` class: owns `Client`, `PollLoop`, offset, the
     registry `std::map<int, Session*>`, and `chat_id -> active id`.
   - New `Session` class (or refactor `RemoteControl` into it): owns
     `fUserMessages`, history provider/appender, turn lock,
     `fActiveChatId`, allowed-tools set, and the work queue/worker.
   - `Dispatcher::Register(Session*) -> int` returns a unique id;
     `Dispatcher::Unregister(int)` removes it.
   - Move the `/sessions`, `/session N`, `/whoami` handling into the
     Dispatcher's update path, *before* per-session routing.

2. **Unique IDs.** A monotonic counter in the Dispatcher (small ints:
   1, 2, 3…). Stable for the life of the session; reused only after a
   session is unregistered. Shown to the phone and the desktop.

3. **`chat_window.{cpp,h}` — GUI.**
   - Move poller ownership **out** of `ChatWindow`. The window no longer
     owns `fRemote`; instead it *registers itself as a Session* with a
     process-wide Dispatcher (held by the `BApplication`).
   - The window registers its own history provider/appender (already
     exists, just re-pointed) and learns its session id (show it in the
     title bar / token bar badge).
   - The Remote toggle becomes "start/stop the **Dispatcher**" (process
     scope) rather than a per-window poller.

4. **`app_main_gui.cpp` — multi-window.**
   - Allow creating **multiple** `ChatWindow`s (File ▸ New Session, or a
     session switcher). Each new window self-registers with the
     Dispatcher and gets its own id.
   - The `ClaudeGuiApp` owns the single `Dispatcher` so all windows
     share one poller/token.
   - Quit when the *last* window closes (standard Haiku app behaviour).

5. **`session.cpp` — CLI parity (optional, later).**
   - The CLI process registers its REPL conversation as a Session with
     the same Dispatcher abstraction. (CLI is single-conversation, so it
     registers exactly one session — but it can still be listed/switched
     to from the phone if a shared dispatcher exists.)

6. **Concurrency model — decide turn locking.**
   - Today: one global turn lock serialises *all* turns.
   - For independent sessions you likely want **per-session** turn locks
     so two phones (or a phone + desktop) can run different sessions
     concurrently. This is the largest behavioural change and the
     riskiest; see "Phasing".

## Phasing (recommended)

**Phase 1 — registry + IDs + switching, single active session. ✅ DONE.**
Implemented as a process-wide `SessionRegistry` (in `telegram.{cpp,h}`)
rather than a full Dispatcher/Session class split, to keep the first
increment low-risk. Each `RemoteControl` registers itself in `Start()`
(unique ascending int ID), unregisters in `Stop()`, and can set a
display title via `SetSessionTitle()`. The poll/work loop handles
`/sessions`, `/session N`, and `/whoami` in `TryHandleSessionCommand()`
*before* turn acquisition, so a phone can list and switch even mid-turn.
Routing uses a per-chat active-session pointer; with a single live
session a chat defaults to it (no `/session` needed). The GUI labels its
session with the conversation topic. Global turn lock unchanged — no
concurrent turns yet. The class rename to Dispatcher/Session is deferred
to Phase 2 when multiple live sessions actually exist.

**Phase 2 — GUI multi-window.** Split into 2a (single process) and 2b
(multi-process broker).

*Phase 2a — multiple windows in one GUI process. ✅ DONE.* Implemented
with a lighter touch than the original sketch: the poller (a single
`RemoteControl`) is still created by whichever window toggles Remote on,
but it now runs as **transport only** — `SetSelfRegister(false)` stops it
registering a session of its own. Instead **each `ChatWindow` registers
itself** as a live session in its constructor (`_RegisterSession`) and
unregisters in its destructor, supplying a history provider (snapshot of
its `fMessages`) and an appender that posts `MSG_REMOTE_APPEND` to *that*
window. A prompt routed to session N therefore runs against window N's
conversation and its reply renders into window N's transcript via the
existing remote-append path (turn-completion granularity; live
token-by-token cross-window streaming via the `sinkFactory` seam is left
as a later refinement). **File ▸ New Session** posts `MSG_NEW_WINDOW` to
`ClaudeGuiApp`, which spawns another self-registering window with the
same auth/config. `QuitRequested` now counts live `ChatWindow`s and only
quits the app when the last one closes. Global turn lock retained — one
turn at a time across all windows, which keeps the cross-window history
access safe without per-session locks.

*Known 2a limitation.* The poller is still owned per-window, so only the
window that toggled Remote on hosts the transport. Toggling Remote on in
a **second** window tries to start a second poller on the same token; the
`Preflight` 409 check catches this and refuses with a clear message
rather than wedging, but it is not seamless. Promoting the single poller
into `ClaudeGuiApp` (so any window can be the one that turns it on, and
all windows share it) is a clean follow-up — and a natural stepping stone
to the 2b broker. Until then: turn Remote on in one window; all other
windows are still listed and routable via `/sessions` / `/session N`.

*Phase 2b — multi-process broker (GUI×N and CLI×N share one bot).* An
in-process registry is a C++ singleton and cannot span processes; two
processes polling the same token 409 each other. So one process must own
the token as a **broker** and the others register their sessions over
IPC. On Haiku the mechanism is `be_roster` + `BMessenger`: a process
discovers the broker by app signature and exchanges `BMessage`s
(register session, routed prompt in, reply chunk out, unregister).
Because 2a already routes through a callback seam, 2b mostly swaps "call
the local callback" for "send a BMessage to the owning process". Open
problems unique to 2b: **leader election** (which instance owns the
token when several launch), **broker crash recovery** (token orphaned —
elect a new owner), and a small **wire protocol**. Deferred until 2a is
proven.

**Phase 3 — per-session concurrency.**
Replace the global turn lock with per-session locks so multiple sessions
can run turns at the same time. Requires auditing every place that
assumes a single in-flight turn (status bar, cursor mirroring, the
`g_interrupted`/cancel flag, the permission queue which is currently one
shared `fPermQueue`).

## Risks & gotchas

- **Permission queue is currently shared** (`fPermQueue`). With multiple
  concurrent sessions, perm:*/choice:* callbacks must be routed to the
  *originating* session, not a global queue. Needs per-session perm
  queues keyed so the Dispatcher can deliver a tap to the right one.
- **`g_interrupted` is repurposed as a per-turn cancel in the GUI.** With
  concurrent sessions this global flag is ambiguous — it would cancel the
  wrong turn. Must become per-session.
- **`g_muted` is global** — fine if mute is process-wide, but if mute
  should be per-session it needs to move into Session.
- **Cursor/terminal mirroring** in `ProcessUpdate` assumes one REPL
  surface (CLI). In the GUI path this is bypassed; keep them separate.
- **Offset ownership** stays in the Dispatcher — never split it.

## Open questions (need answers before coding)

1. **Concurrency goal:** switch-only (one active turn at a time) or
   true concurrent turns across sessions? This decides whether Phase 3
   is in scope. *(Phase 1 is switch-only and is enough to validate the
   UX.)*
2. **Cross-process dispatcher?** If the desktop GUI and a separate CLI
   process should appear as sibling sessions on the phone, the
   Dispatcher must be shared across *processes* (a local socket / single
   "broker" process that owns the token). That is a much bigger lift
   than a per-process dispatcher. If multi-session is GUI-only
   (multiple windows in one GUI process), a per-process dispatcher
   suffices.
3. **Session identity persistence:** should a session id survive a
   restart (tie to the saved session file), or is it ephemeral per run?
4. **Default routing:** when a chat hasn't picked a session yet, attach
   to newest, or require an explicit `/session N` first?

## Recommendation

Start with **Phase 1** (Dispatcher/Session split + `/sessions`,
`/session N`, `/whoami`, single active session, global turn lock). It is
self-contained, low-risk, and immediately gives the phone the
list/switch UX you want. Decide the **cross-process question (Open
Question #2)** before Phase 2, because it changes whether the dispatcher
lives in `ClaudeGuiApp` or in a separate broker.
