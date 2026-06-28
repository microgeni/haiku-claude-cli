// editor_integration.cpp — Genio IDE round-trip (see editor_integration.h).

#include "editor_integration.h"

#include <atomic>
#include <cstring>

#include "config.h"

#ifdef __HAIKU__
#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <Roster.h>
#endif

namespace editor {

namespace {

// Process-wide flag. Set once by the GUI in ArgvReceived(); read on the
// worker thread by NotifyFileChanged(). std::atomic keeps that race clean.
std::atomic<bool> sLaunchedFromGenio{false};

} // namespace


void
SetLaunchedFromGenio(bool launched)
{
	sLaunchedFromGenio.store(launched, std::memory_order_relaxed);
}


bool
LaunchedFromGenio()
{
	return sLaunchedFromGenio.load(std::memory_order_relaxed);
}


#ifdef __HAIKU__

void
OpenInGenio(const std::string& path, int line)
{
	if (path.empty())
		return;

	entry_ref ref;
	status_t status = get_ref_for_path(path.c_str(), &ref);
	if (status != B_OK) {
		config::LogLine("OpenInGenio: get_ref_for_path failed for '" + path
			+ "' (" + strerror(status) + ")");
		return;
	}

	// Build the open request the way Genio's RefsReceived expects it: a
	// B_REFS_RECEIVED with the file ref, plus an optional be:line to jump
	// the cursor (Genio reads be:line / start:line).
	BMessage refs(B_REFS_RECEIVED);
	refs.AddRef("refs", &ref);
	if (line >= 1)
		refs.AddInt32("be:line", line);

	BRoster roster;
	if (roster.IsRunning(kGenioSignature)) {
		// Genio launched us, so it is normally already running: route the
		// ref straight to the live instance instead of starting a new one.
		BMessenger genio(kGenioSignature);
		status = genio.SendMessage(&refs);
	} else {
		status = roster.Launch(kGenioSignature, &refs);
	}

	if (status != B_OK && status != B_ALREADY_RUNNING) {
		config::LogLine("OpenInGenio: could not reach Genio for '" + path
			+ "' (" + strerror(status) + ")");
	}
}

#else // !__HAIKU__

void
OpenInGenio(const std::string&, int)
{
	// No BRoster off Haiku; the integration is a no-op on the dev platform.
}

#endif // __HAIKU__


void
NotifyFileChanged(const std::string& toolName, const std::string& path, int line)
{
	if (!LaunchedFromGenio())
		return;
	if (toolName != "Write" && toolName != "Edit")
		return;
	OpenInGenio(path, line);
}

} // namespace editor
