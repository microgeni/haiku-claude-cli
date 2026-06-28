// editor_integration.h — Genio IDE round-trip.
//
// When the Claude GUI is launched via Genio's Tools ▸ Claude menu, Genio
// passes the active project/file on argv (--project-dir / --file / --line)
// and chdirs into the project. The GUI detects that and calls
// SetLaunchedFromGenio(true). From then on, every file Claude writes or
// edits is opened (or refreshed) in the live Genio editor via a
// B_REFS_RECEIVED message to Genio's application signature.
//
// The seam lives in the shared core so api.cpp can call NotifyFileChanged()
// after a successful Write/Edit without depending on the GUI. It is inert
// (a no-op) unless SetLaunchedFromGenio(true) was called, so the CLI and a
// normally-launched GUI are completely unaffected.

#ifndef EDITOR_INTEGRATION_H
#define EDITOR_INTEGRATION_H

#include <string>

namespace editor {

// Genio's application signature, registered in Haiku's MIME database.
inline constexpr const char* kGenioSignature = "application/x-vnd.Genio";

// Record whether this process was started by Genio (Tools ▸ Claude). The
// GUI sets this true in ArgvReceived() when Genio's --project-dir / --file
// argument is present. Defaults to false, so the integration stays off for
// the CLI and for a GUI the user launched directly.
void SetLaunchedFromGenio(bool launched);
bool LaunchedFromGenio();

// Called by the tool-dispatch loop after a Write or Edit tool succeeds.
// When LaunchedFromGenio() is true and the build is Haiku, this opens
// `path` in the running Genio instance, optionally jumping to `line`
// (1-based; 0 = no jump). Best-effort: failures are logged and swallowed
// so editing never blocks on the IDE. A no-op on non-Haiku builds or when
// the integration is disabled.
void NotifyFileChanged(const std::string& toolName,
                       const std::string& path,
                       int line = 0);

// Open a single file in Genio directly. Exposed for callers that already
// know they want IDE behaviour (e.g. a future "Open in Genio" menu item).
void OpenInGenio(const std::string& path, int line = 0);

} // namespace editor

#endif // EDITOR_INTEGRATION_H
