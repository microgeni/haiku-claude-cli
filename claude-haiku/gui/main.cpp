// GUI front-end stub — Step 4 of the architecture plan.
// This file will become BApplication + ChatWindow once the core library
// and StreamSink seam are in place (Steps 1-3).

#ifdef __HAIKU__
#include <Application.h>
#include "gui/ChatWindow.h"

int main()
{
	BApplication app("application/x-vnd.haiku-claude-gui");
	ChatWindow* win = new ChatWindow();
	win->Show();
	app.Run();
	return 0;
}
#else
#include <cstdio>
int main()
{
	std::fprintf(stderr, "claude-gui requires Haiku (BeAPI)\n");
	return 1;
}
#endif
