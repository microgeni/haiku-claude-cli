#include <Alert.h>
#include <Application.h>
#include <String.h>

#include "chat_window.h"
#include "config.h"
#include "hooks.h"
#include "mcp.h"
#include "paths.h"

// Application signature registered in Haiku's MIME database.
// Must match the BEOS:APP_SIG attribute stamped onto the binary.
static const char* kAppSig = "application/x-vnd.Microgeni-claude-gui";

class ClaudeGuiApp : public BApplication {
public:
	ClaudeGuiApp() : BApplication(kAppSig) {}

	void ReadyToRun() override
	{
		// Load config the same way the CLI does — this picks up model,
		// max_tokens, system prompt, hooks, and MCP servers.
		const config::Config cfg = config::Load();
		config::InitLogging(cfg.logging_enabled);
		hooks::Load(cfg.hooks);
		mcp::Init(cfg.mcp_servers);

		// Resolve authentication (OAuth tokens or ANTHROPIC_API_KEY).
		const config::Auth auth = config::ResolveAuth();
		if (auth.kind == config::AuthKind::None) {
			BAlert* alert = new BAlert("Authentication Required",
			    "No Claude authentication found.\n\n"
			    "Run  claude login  in a Terminal to authenticate\n"
			    "with your Claude account, or set ANTHROPIC_API_KEY\n"
			    "in your environment.",
			    "Quit", nullptr, nullptr,
			    B_WIDTH_AS_USUAL, B_STOP_ALERT);
			alert->Go();
			PostMessage(B_QUIT_REQUESTED);
			return;
		}

		const std::string model      = cfg.model;
		const int         maxTokens  = cfg.max_tokens;
		const std::string systemPmt  = cfg.system;

		ChatWindow* win = new ChatWindow(auth, model, maxTokens, systemPmt);
		win->Show();
	}
};

int main()
{
	ClaudeGuiApp app;
	app.Run();
	return 0;
}
