// ipc_test_sender — standalone Haiku app that exercises the Genio→Claude IPC
// path without requiring Genio to be running.
//
// Protocol summary (canonical: src/app_main_gui.cpp kMsgAskPrompt / 'ASKP',
// documented in docs/IPC.md):
//   Target app signature : application/x-vnd.Microgeni-claude-gui
//   BMessage::what       : 'ASKP'
//   Fields:
//     "prompt"      B_STRING_TYPE  — the question/instruction (required)
//     "context"     B_STRING_TYPE  — extra context appended below the prompt (optional)
//     "working_dir" B_STRING_TYPE  — override working directory for tool calls (optional)
//
// Usage:
//   ipc_test_sender [--prompt "…"] [--context "…"] [--working-dir /path]
//
// The Claude GUI app must already be running before this is invoked.
// The window will be brought to the front and the input field will be
// pre-filled with the assembled prompt — ready for the user to review and send.

#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <Roster.h>
#include <String.h>

#include <cstdio>
#include <cstring>
#include <string>

// Must match the signature in app_main_gui.cpp / Makefile GUI_APP_SIG.
static const char* kClaudeGuiSig = "application/x-vnd.Microgeni-claude-gui";

// Must match gui::kMsgAskPrompt in src/app_main_gui.cpp.
static const uint32_t kMsgAskPrompt = 'ASKP';

static void print_usage(const char* argv0)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  --prompt     TEXT   Question / instruction to send (required)\n"
		"  --context    TEXT   Extra context prepended before the prompt (optional)\n"
		"  --working-dir PATH  Override Claude's working directory (optional)\n"
		"  --help              Print this help\n"
		"\n"
		"The Claude GUI app (application/x-vnd.Microgeni-claude-gui) must be\n"
		"running before this tool is invoked.\n",
		argv0);
}

int main(int argc, char** argv)
{
	std::string prompt;
	std::string context;
	std::string workingDir;

	// ── Argument parsing ─────────────────────────────────────────────────────
	for (int i = 1; i < argc; ++i) {
		const char* arg = argv[i];

		if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		}

		auto next = [&](const char* flag) -> const char* {
			if (i + 1 >= argc) {
				fprintf(stderr, "error: %s requires an argument\n", flag);
				return nullptr;
			}
			return argv[++i];
		};

		if (std::strcmp(arg, "--prompt") == 0) {
			const char* v = next("--prompt");
			if (!v) return 1;
			prompt = v;
		} else if (std::strcmp(arg, "--context") == 0) {
			const char* v = next("--context");
			if (!v) return 1;
			context = v;
		} else if (std::strcmp(arg, "--working-dir") == 0) {
			const char* v = next("--working-dir");
			if (!v) return 1;
			workingDir = v;
		} else {
			fprintf(stderr, "error: unknown argument '%s'\n", arg);
			print_usage(argv[0]);
			return 1;
		}
	}

	// ── Validate ─────────────────────────────────────────────────────────────
	if (prompt.empty()) {
		// Default demo prompt when run with no arguments.
		prompt = "Hello from ipc_test_sender! What is 2 + 2?";
		fprintf(stderr,
			"[ipc_test_sender] No --prompt given — using demo: \"%s\"\n",
			prompt.c_str());
	}

	// ── Bootstrap a minimal BApplication so BMessenger works ─────────────────
	// We use a unique signature for the test app to avoid confusing the roster.
	BApplication app("application/x-vnd.Microgeni-claude-ipc-test");

	// ── Check the Claude GUI is running ─────────────────────────────────────
	BMessenger target(kClaudeGuiSig);
	if (!target.IsValid()) {
		fprintf(stderr,
			"error: Claude GUI app ('%s') is not running.\n"
			"  Please launch the Claude app first, then re-run this test.\n",
			kClaudeGuiSig);
		return 1;
	}

	// ── Build the IPC message ─────────────────────────────────────────────────
	BMessage msg(kMsgAskPrompt);
	msg.AddString("prompt", prompt.c_str());
	if (!context.empty())
		msg.AddString("context", context.c_str());
	if (!workingDir.empty())
		msg.AddString("working_dir", workingDir.c_str());

	// ── Send (fire-and-forget, no reply expected) ─────────────────────────────
	status_t err = target.SendMessage(&msg);
	if (err != B_OK) {
		fprintf(stderr, "error: BMessenger::SendMessage failed: %s (0x%08lx)\n",
			strerror(err), (unsigned long)err);
		return 1;
	}

	// Print a summary of what was sent.
	fprintf(stdout, "[ipc_test_sender] Message sent successfully.\n");
	fprintf(stdout, "  prompt      : %s\n", prompt.c_str());
	if (!context.empty())
		fprintf(stdout, "  context     : %s\n", context.c_str());
	if (!workingDir.empty())
		fprintf(stdout, "  working_dir : %s\n", workingDir.c_str());
	fprintf(stdout,
		"\nThe Claude window should have come to the front with the prompt\n"
		"pre-filled in the input box — press Enter (or Send) to submit it.\n");

	return 0;
}
