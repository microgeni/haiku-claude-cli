#ifndef HAIKU_CLAUDE_CLI_SESSION_STORE_H
#define HAIKU_CLAUDE_CLI_SESSION_STORE_H

// session_store — BFS-native conversation persistence for claude-gui.
//
// Each conversation is stored as a flat file under
//   ~/config/settings/claude-cli/sessions/YYYYMMDD-HHMMSS-<slug>.session
//
// File body: BMessage::Flatten() of a BMessage containing the full
// messages JSON as a B_STRING_TYPE field named "messages".
//
// BFS extended attributes on each file (all in the claude: namespace):
//   claude:title    (string)  — first user message, truncated to 80 chars
//   claude:model    (string)  — model used for this session
//   claude:turns    (int32)   — number of completed turns
//   claude:created  (int64)   — creation time (unix seconds)
//   claude:modified (int64)   — last-modified time (unix seconds)
//
// Because the attributes are indexed (by the install script / first run),
// the SessionPanel can do instant BFS queries like:
//   claude:title contains "haiku"
//   claude:model == "claude-sonnet-4-5"
//
// The files are ordinary Haiku files — the user can browse, rename,
// copy, and delete them in Tracker just like any document. Double-
// clicking one will eventually launch claude-gui with that session
// loaded (RefsReceived, future work).

#include <string>
#include <vector>
#include <ctime>

#include <nlohmann/json.hpp>

namespace session {

// Metadata about one saved session — read from BFS attributes.
// Cheap to collect: no file body is read to populate this.
struct SessionInfo {
	std::string path;      // absolute path to the .session file
	std::string title;     // claude:title
	std::string model;     // claude:model
	int32_t     turns    = 0;
	time_t      created  = 0;
	time_t      modified = 0;
};

// Per-session settings restored when a session is loaded. Stored in the
// file body (BMessage envelope), not as BFS attributes, since the system
// prompt and working dir can be long and don't need indexing.
struct SessionSettings {
	std::string model;
	std::string systemPrompt;
	std::string workingDir;
	int         maxTokens = 0;   // 0 = unset (caller keeps its default)
};

// Save (or overwrite) a session file for the given conversation.
// Creates the sessions directory if it does not exist.
// Returns the path written, or empty string on failure.
std::string Save(const std::string& existingPath,   // "" = create new file
                 const std::string& title,
                 const std::string& model,
                 int                turns,
                 const nlohmann::json& messages,
                 const SessionSettings& settings = {});

// Load the messages JSON from a .session file.
// Returns an empty array on failure.
nlohmann::json Load(const std::string& path);

// Load the per-session settings (model, system prompt, working dir,
// max-tokens) saved alongside the conversation. Fields absent in older
// session files are left at their struct defaults. Returns false only if
// the file can't be read at all.
bool LoadSettings(const std::string& path, SessionSettings& out);

// Delete a .session file. Returns true on success (or if it was already
// gone). Used by the GUI session sidebar.
bool Delete(const std::string& path);

// List all sessions in the sessions directory, sorted by modified
// time descending (most recent first).
std::vector<SessionInfo> List();

// Ensure the BFS attribute indexes exist for fast queries.
// Safe to call on every launch — addindex is idempotent.
void EnsureIndexes();

} // namespace session

#endif // HAIKU_CLAUDE_CLI_SESSION_STORE_H
