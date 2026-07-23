#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <cstdlib>
#include <string>

#include "chat_window.h"
#include "agents.h"
#include "api.h"
#include "commands.h"
#include "config.h"
#include "editor_integration.h"
#include "hooks.h"
#include "mcp.h"
#include "oauth.h"
#include "paths.h"
#include "skills.h"

// Application signature registered in Haiku's MIME database.
// Must match the BEOS:APP_SIG attribute stamped onto the binary.
static const char* kAppSig = "application/x-vnd.Microgeni-claude-gui";

// ---------------------------------------------------------------------------
// AuthWindow — modal sign-in dialog shown when no credentials are found.
//
// Offers two paths, mirroring the CLI:
//   • API key — paste an ANTHROPIC_API_KEY; saved 0600 via SaveApiKey().
//   • OAuth   — "Open browser" runs BuildAuthUrl() + launches the browser;
//               the user pastes the redirect code, which ExchangeCode()
//               trades for tokens (saved to the credentials file).
//
// Runs its own nested loop via a semaphore (the app has no window yet).
// Go() returns true if authentication now resolves.
// ---------------------------------------------------------------------------
class AuthWindow : public BWindow {
public:
	AuthWindow()
		: BWindow(BRect(0, 0, 460, 300), "Sign in to Claude",
		          B_TITLED_WINDOW,
		          B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
		  fDoneSem(create_sem(0, "auth_modal"))
	{
		BStringView* intro = new BStringView("intro",
			"No Claude credentials found. Sign in to continue.");

		// ── API key section ──────────────────────────────────────────
		fKeyField = new BTextControl("apikey", "API key:", "",
		                             new BMessage(MSG_SAVE_KEY));
		if (BTextView* tv = fKeyField->TextView())
			tv->HideTyping(true);    // mask the key like a password field
		BButton* saveKey = new BButton("savekey", "Save & Continue",
		                               new BMessage(MSG_SAVE_KEY));

		// ── OAuth section ─────────────────────────────────────────────
		BStringView* oauthLbl = new BStringView("oauthlbl",
			"Or sign in with your Claude account (Pro/Max):");
		BButton* openBrowser = new BButton("openbrowser",
			"Open browser to authorize\xE2\x80\xA6",
			new BMessage(MSG_OPEN_BROWSER));
		fCodeField = new BTextControl("code", "Paste code:", "",
		                              new BMessage(MSG_FINISH_OAUTH));
		fFinishBtn = new BButton("finish", "Complete Login",
		                         new BMessage(MSG_FINISH_OAUTH));
		fFinishBtn->SetEnabled(false);   // enabled after the browser opens

		BButton* quit = new BButton("quit", "Quit",
		                            new BMessage(MSG_AUTH_QUIT));

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(intro)
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
				.Add(fKeyField)
				.Add(saveKey)
			.End()
			.AddStrut(B_USE_SMALL_SPACING)
			.Add(oauthLbl)
			.Add(openBrowser)
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
				.Add(fCodeField)
				.Add(fFinishBtn)
			.End()
			.AddStrut(B_USE_SMALL_SPACING)
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
				.AddGlue()
				.Add(quit)
			.End()
		.End();

		saveKey->MakeDefault(true);
		CenterOnScreen();
	}

	~AuthWindow() override { delete_sem(fDoneSem); }

	void MessageReceived(BMessage* msg) override
	{
		switch (msg->what) {
			case MSG_SAVE_KEY:    _SaveKey();      break;
			case MSG_OPEN_BROWSER:_OpenBrowser();  break;
			case MSG_FINISH_OAUTH:_FinishOAuth();  break;
			case MSG_AUTH_QUIT:   _Finish(false);  break;
			default: BWindow::MessageReceived(msg); break;
		}
	}

	bool QuitRequested() override { _Finish(false); return true; }

	// Show modally; returns true once authentication resolves.
	bool Go()
	{
		Show();
		acquire_sem(fDoneSem);
		const bool ok = fSucceeded;
		if (Lock()) Quit();
		return ok;
	}

private:
	enum {
		MSG_SAVE_KEY     = 'aSky',
		MSG_OPEN_BROWSER = 'aObr',
		MSG_FINISH_OAUTH = 'aFin',
		MSG_AUTH_QUIT    = 'aQit',
	};

	void _Note(const char* title, const char* body, alert_type t = B_INFO_ALERT)
	{
		BAlert* a = new BAlert(title, body, "OK", nullptr, nullptr,
		                       B_WIDTH_AS_USUAL, t);
		a->Go();
	}

	void _SaveKey()
	{
		const char* k = fKeyField->Text();
		if (!k || !k[0]) {
			_Note("API key", "Please paste an API key first.");
			return;
		}
		if (!config::SaveApiKey(k)) {
			_Note("API key", "Could not save the key.", B_WARNING_ALERT);
			return;
		}
		_Finish(true);
	}

	void _OpenBrowser()
	{
		if (!BuildAuthUrl(fAuthUrl, fVerifier, fState) || fAuthUrl.empty()) {
			_Note("Login", "Could not start the login flow.", B_WARNING_ALERT);
			return;
		}
		const std::string cmd = "open " + config::ShellSingleQuote(fAuthUrl) + " >/dev/null 2>&1 &";
		std::system(cmd.c_str());  // flawfinder: ignore
		fFinishBtn->SetEnabled(true);
		fCodeField->MakeFocus(true);
		_Note("Login",
			"Your browser was opened to authorize. After approving, copy the\n"
			"code from the redirect page and paste it into 'Paste code', then\n"
			"click Complete Login.");
	}

	void _FinishOAuth()
	{
		const char* c = fCodeField->Text();
		if (!c || !c[0]) {
			_Note("Login", "Paste the code from the redirect page first.");
			return;
		}
		auto tokens = ExchangeCode(c, fVerifier, fState);
		if (!tokens) {
			_Note("Login",
				"Token exchange failed. Double-check the pasted code "
				"(paste the whole 'code#state' value) and try again.",
				B_WARNING_ALERT);
			return;
		}
		_Finish(true);
	}

	void _Finish(bool ok)
	{
		if (!fFinished) {
			fFinished   = true;
			fSucceeded  = ok;
			release_sem(fDoneSem);
		}
	}

	BTextControl* fKeyField  = nullptr;
	BTextControl* fCodeField = nullptr;
	BButton*      fFinishBtn = nullptr;
	std::string   fAuthUrl, fVerifier, fState;
	sem_id        fDoneSem;
	bool          fFinished  = false;
	bool          fSucceeded = false;
};

class ClaudeGuiApp : public BApplication {
public:
	ClaudeGuiApp() : BApplication(kAppSig) {}

	// Called by the app server when the binary is launched with arguments.
	// Parses -w / --working-dir <path> and stores it for ReadyToRun().
	//
	// Genio's Tools ▸ Claude extension launcher also passes the active
	// project and file: --project-dir <path>, --file <path>, --line <n>.
	// Their presence means we were started by Genio, so we mark the editor
	// integration live (Claude's file writes/edits open back in Genio) and
	// adopt --project-dir as the working directory.
	void ArgvReceived(int32 argc, char** argv) override
	{
		auto next = [&](int32& i) -> std::string {
			return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
		};

		for (int32 i = 1; i < argc; i++) {
			std::string arg = argv[i];
			if ((arg == "-w" || arg == "--working-dir") && i + 1 < argc) {
				fWorkingDir = argv[++i];
			} else if (arg.rfind("--working-dir=", 0) == 0) {
				fWorkingDir = arg.substr(14);
			} else if (arg == "--project-dir") {
				fWorkingDir = next(i);
				fFromGenio  = true;
			} else if (arg.rfind("--project-dir=", 0) == 0) {
				fWorkingDir = arg.substr(14);
				fFromGenio  = true;
			} else if (arg == "--file" || arg.rfind("--file=", 0) == 0) {
				// Genio passes the focused file; keep it as provenance and
				// remember it so it can seed the prompt / working dir.
				fFile = (arg == "--file") ? next(i) : arg.substr(7);
				fFromGenio = true;
			} else if (arg == "--line" || arg.rfind("--line=", 0) == 0) {
				fLine = (arg == "--line") ? next(i) : arg.substr(7);
			} else if (arg == "--prompt" || arg.rfind("--prompt=", 0) == 0) {
				// Initial prompt to seed the input box (e.g. Genio's
				// "Ask Claude to fix this"). Not auto-sent from the CLI so
				// the user can review it first.
				fInitialPrompt = (arg == "--prompt") ? next(i) : arg.substr(9);
			} else if (arg == "--send") {
				// Auto-submit the seeded --prompt without waiting for Enter.
				fAutoSend = true;
			}
		}

		// Diagnostic: stash the raw argv so ReadyToRun can log it once
		// logging is initialized (LogLine is a no-op before InitLogging,
		// which runs after ArgvReceived).
		fRawArgv.clear();
		for (int32 i = 0; i < argc; i++) {
			if (i) fRawArgv += ' ';
			fRawArgv += argv[i];
		}

		// Apply the flag here as well as in ReadyToRun: on a re-launch into
		// an already-running instance ReadyToRun does not run again, so
		// setting it only there would miss the Genio provenance.
		if (fFromGenio)
			editor::SetLaunchedFromGenio(true);
	}

	void ReadyToRun() override
	{
		// Load config the same way the CLI does — this picks up model,
		// max_tokens, system prompt, hooks, and MCP servers.
		const config::Config cfg = config::Load();
		config::InitLogging(cfg.logging_enabled);
		config::SetHistoryMessageCap(cfg.history_max_messages);
		config::LogLine("gui ReadyToRun fromGenio="
			+ std::string(fFromGenio ? "yes" : "no")
			+ " argv=[" + fRawArgv + "]");
		hooks::Load(cfg.hooks);
		mcp::Init(cfg.mcp_servers);

		// Load custom commands, Agent Skills, and subagents so the GUI
		// has the same /skill-name expansions, model-invocable skills,
		// and Task-delegated subagents as the CLI.
		commands::Load(paths::ConfigDir() + "/commands");
		skills::Load(paths::UserSkillsDir(), paths::ProjectSkillsDir());
		agents::Load(paths::UserAgentsDir(), paths::ProjectAgentsDir());

		// Resolve authentication (OAuth tokens, ANTHROPIC_API_KEY, or a
		// stored API key). If none is found, show the in-app sign-in
		// dialog and re-resolve once it closes.
		config::Auth auth = config::ResolveAuth();
		if (auth.kind == config::AuthKind::None) {
			AuthWindow* login = new AuthWindow();
			const bool signedIn = login->Go();    // blocks until closed
			if (!signedIn) {
				PostMessage(B_QUIT_REQUESTED);
				return;
			}
			auth = config::ResolveAuth();
			if (auth.kind == config::AuthKind::None) {
				BAlert* alert = new BAlert("Authentication",
				    "Sign-in did not complete. Please try again.",
				    "Quit", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
				alert->Go();
				PostMessage(B_QUIT_REQUESTED);
				return;
			}
		}

		// If no -w flag was given, fall back to the environment variable.
		if (fWorkingDir.empty()) {
			const char* env = std::getenv("CLAUDE_WORKING_DIR");  // flawfinder: ignore (null-checked; used only as a path)
			if (env != nullptr)
				fWorkingDir = env;
		}

		// Activate the Genio round-trip if we were launched from its
		// Tools ▸ Claude menu. From here on, files Claude writes or edits
		// are opened back in the live Genio editor.
		editor::SetLaunchedFromGenio(fFromGenio);

		// Remember the spawn parameters so File ▸ New Session can create
		// additional windows with the same auth/config.
		fAuth      = auth;
		fModel     = cfg.model;
		fMaxTokens = cfg.max_tokens;
		fSystemPmt = cfg.system;  // flawfinder: ignore (member read, not the system() call)
		fNotifyMin = static_cast<int>(cfg.notify_min_duration_sec);

		_SpawnWindow();
	}

	// Create one chat window with the stored spawn parameters and show it.
	// Each window self-registers as a live remote-control session in its
	// constructor, so the phone can list/switch between them. Used for the
	// initial window and every File ▸ New Session.
	void _SpawnWindow()
	{
		ChatWindow* win = new ChatWindow(fAuth, fModel, fMaxTokens, fSystemPmt,
		                                 fNotifyMin, fWorkingDir,
		                                 fInitialPrompt, fAutoSend);
		win->Show();

		// The initial prompt is one-shot: consume it so a later
		// File ▸ New Session (which also calls _SpawnWindow) opens blank.
		fInitialPrompt.clear();
		fAutoSend = false;
	}

	void MessageReceived(BMessage* msg) override
	{
		switch (msg->what) {
			case gui::MSG_NEW_WINDOW:
				_SpawnWindow();
				break;

			// 'ASKP' — programmatic "ask Claude" from another app (e.g.
			// Genio's "Ask Claude to fix this"). Carries optional fields:
			//   "prompt"      the question / instruction (seeds the input box)
			//   "context"     extra text appended below the prompt
			//   "working_dir" project root / working directory
			//   "file"        focused file (provenance)
			//   "line"        1-based caret line
			//   "send"        bool: auto-submit instead of waiting for Enter
			// A new window is opened scoped to working_dir with the prompt
			// prefilled. This is the direct alternative to the clipboard.
			case kMsgAskPrompt:
			{
				const char* prompt = nullptr;
				if (msg->FindString("prompt", &prompt) == B_OK && prompt) {
					fInitialPrompt = prompt;
					const char* ctx = nullptr;
					if (msg->FindString("context", &ctx) == B_OK && ctx
					    && ctx[0] != '\0') {
						fInitialPrompt += "\n\n";
						fInitialPrompt += ctx;
					}
				}
				const char* wd = nullptr;
				if (msg->FindString("working_dir", &wd) == B_OK && wd
				    && wd[0] != '\0') {
					fWorkingDir = wd;
					fFromGenio  = true;
					editor::SetLaunchedFromGenio(true);
				}
				const char* file = nullptr;
				if (msg->FindString("file", &file) == B_OK && file
				    && file[0] != '\0') {
					fFile      = file;
					fFromGenio = true;
					editor::SetLaunchedFromGenio(true);
				}
				bool send = false;
				if (msg->FindBool("send", &send) == B_OK)
					fAutoSend = send;

				_SpawnWindow();
				break;
			}

			default:
				BApplication::MessageReceived(msg);
				break;
		}
	}

	// BMessage 'what' accepted from external apps to open a prompt.
	static constexpr uint32 kMsgAskPrompt = 'ASKP';

private:
	std::string    fWorkingDir; // resolved from -w / --working-dir or env.
	bool           fFromGenio = false; // launched via Genio Tools ▸ Claude.
	std::string    fRawArgv;    // joined argv, logged once in ReadyToRun.
	std::string    fFile;       // --file: focused file (provenance).
	std::string    fLine;       // --line: 1-based caret line.
	std::string    fInitialPrompt; // --prompt / 'ASKP': seed for input box.
	bool           fAutoSend = false;  // --send / 'ASKP' send=true.
	config::Auth   fAuth;       // spawn parameters captured in ReadyToRun.
	std::string    fModel;
	int            fMaxTokens = 0;
	std::string    fSystemPmt;
	int            fNotifyMin = 0;
};

int main()
{
	api::GlobalInit();
	ClaudeGuiApp app;
	app.Run();
	return 0;
}
