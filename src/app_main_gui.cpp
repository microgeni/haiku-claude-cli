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
#include "commands.h"
#include "config.h"
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
		const std::string cmd = "open '" + fAuthUrl + "' >/dev/null 2>&1 &";
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
	void ArgvReceived(int32 argc, char** argv) override
	{
		for (int32 i = 1; i < argc; i++) {
			std::string arg = argv[i];
			if ((arg == "-w" || arg == "--working-dir") && i + 1 < argc) {
				fWorkingDir = argv[++i];
			} else if (arg.rfind("--working-dir=", 0) == 0) {
				fWorkingDir = arg.substr(14);
			}
		}
	}

	void ReadyToRun() override
	{
		// Load config the same way the CLI does — this picks up model,
		// max_tokens, system prompt, hooks, and MCP servers.
		const config::Config cfg = config::Load();
		config::InitLogging(cfg.logging_enabled);
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
			const char* env = std::getenv("CLAUDE_WORKING_DIR");
			if (env != nullptr)
				fWorkingDir = env;
		}

		const std::string model      = cfg.model;
		const int         maxTokens  = cfg.max_tokens;
		const std::string systemPmt  = cfg.system;
		const int         notifyMin  = static_cast<int>(cfg.notify_min_duration_sec);

		ChatWindow* win = new ChatWindow(auth, model, maxTokens, systemPmt,
		                                 notifyMin, fWorkingDir);
		win->Show();
	}

private:
	std::string fWorkingDir; // resolved from -w / --working-dir or CLAUDE_WORKING_DIR
};

int main()
{
	ClaudeGuiApp app;
	app.Run();
	return 0;
}
