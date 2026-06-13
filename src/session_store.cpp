// session_store.cpp — BFS-native conversation persistence.
//
// Storage layout:
//   ~/config/settings/claude-cli/sessions/YYYYMMDD-HHMMSS-<slug>.session
//
// File body: BMessage flattened to disk via BMessage::Flatten(BDataIO*).
// BFS attributes: claude:title, claude:model, claude:turns,
//                 claude:created, claude:modified.

#include "session_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifdef __HAIKU__
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <String.h>
#include <fs_attr.h>
#include <fs_index.h>
#include <Volume.h>
#include <VolumeRoster.h>
#endif

#include "paths.h"

namespace session {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// MIME type stamped on every .session file so Tracker shows a nice icon.
static const char* kSessionMime = "application/x-vnd.claude-session";

// BFS attribute names.
static const char* kAttrTitle    = "claude:title";
static const char* kAttrModel    = "claude:model";
static const char* kAttrTurns    = "claude:turns";
static const char* kAttrCreated  = "claude:created";
static const char* kAttrModified = "claude:modified";

// Build a filesystem-safe slug from the first few words of `title`.
std::string MakeSlug(const std::string& title)
{
	std::string slug;
	int words = 0;
	bool inWord = false;
	for (unsigned char c : title) {
		if (std::isalnum(c)) {
			slug += static_cast<char>(std::tolower(c));
			inWord = true;
		} else if (inWord) {
			slug += '-';
			inWord = false;
			if (++words >= 5) break;
		}
	}
	// Strip trailing dash.
	while (!slug.empty() && slug.back() == '-') slug.pop_back();
	if (slug.empty()) slug = "session";
	if (slug.size() > 40) slug.resize(40);
	return slug;
}

// Format time_t as "YYYYMMDD-HHMMSS".
std::string FormatTimestamp(time_t t)
{
	struct tm tm_buf;
	localtime_r(&t, &tm_buf);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);
	return std::string(buf);
}

#ifdef __HAIKU__
// Write a string BFS attribute to a BNode.
void WriteStrAttr(BNode& node, const char* name, const std::string& value)
{
	node.WriteAttr(name, B_STRING_TYPE, 0,
	               value.c_str(), value.size() + 1);
}

// Write an int32 BFS attribute.
void WriteInt32Attr(BNode& node, const char* name, int32_t value)
{
	node.WriteAttr(name, B_INT32_TYPE, 0, &value, sizeof(value));
}

// Write an int64 BFS attribute.
void WriteInt64Attr(BNode& node, const char* name, int64_t value)
{
	node.WriteAttr(name, B_INT64_TYPE, 0, &value, sizeof(value));
}

// Read a string BFS attribute from a BNode. Returns "" on failure.
std::string ReadStrAttr(BNode& node, const char* name)
{
	attr_info info;
	if (node.GetAttrInfo(name, &info) != B_OK) return {};
	if (info.type != B_STRING_TYPE || info.size <= 0) return {};
	std::string val(static_cast<size_t>(info.size), '\0');
	node.ReadAttr(name, B_STRING_TYPE, 0, &val[0], static_cast<size_t>(info.size));
	// Strip the null terminator that was stored.
	while (!val.empty() && val.back() == '\0') val.pop_back();
	return val;
}

// Read an int32 BFS attribute. Returns 0 on failure.
int32_t ReadInt32Attr(BNode& node, const char* name)
{
	int32_t v = 0;
	node.ReadAttr(name, B_INT32_TYPE, 0, &v, sizeof(v));
	return v;
}

// Read an int64 BFS attribute. Returns 0 on failure.
int64_t ReadInt64Attr(BNode& node, const char* name)
{
	int64_t v = 0;
	node.ReadAttr(name, B_INT64_TYPE, 0, &v, sizeof(v));
	return v;
}

// Stamp MIME type on a file via BNodeInfo.
void StampMime(const std::string& path)
{
	BFile file(path.c_str(), B_READ_WRITE);
	if (file.InitCheck() != B_OK) return;
	BNodeInfo info(&file);
	info.SetType(kSessionMime);
}
#endif // __HAIKU__

} // namespace


// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

std::string Save(const std::string& existingPath,
                 const std::string& title,
                 const std::string& model,
                 int                turns,
                 const nlohmann::json& messages,
                 const SessionSettings& settings)
{
#ifndef __HAIKU__
	(void)existingPath; (void)title; (void)model;
	(void)turns; (void)messages; (void)settings;
	return {};
#else
	// Ensure sessions directory exists.
	const std::string dir = paths::SessionsDir();
	paths::MkdirP(dir);

	// Determine file path — reuse existing or create new.
	std::string filePath = existingPath;
	time_t now = std::time(nullptr);

	if (filePath.empty()) {
		const std::string ts   = FormatTimestamp(now);
		const std::string slug = MakeSlug(title);
		filePath = dir + "/" + ts + "-" + slug + ".session";
	}

	// Serialise messages JSON into a BMessage, then flatten to file.
	const std::string json_str = messages.dump();

	BFile file(filePath.c_str(),
	           B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK) {
		std::fprintf(stderr, "session: cannot open %s\n", filePath.c_str());
		return {};
	}

	BMessage envelope('SESS');
	envelope.AddString("messages", json_str.c_str());
	envelope.AddString("model",    model.c_str());
	envelope.AddInt32 ("turns",    static_cast<int32_t>(turns));
	// Per-session settings — restored on load. Empty/zero fields are
	// still written so loaders can distinguish "explicitly cleared"
	// from "absent in an older file" (FindString fails on absent).
	envelope.AddString("system_prompt", settings.systemPrompt.c_str());
	envelope.AddString("working_dir",   settings.workingDir.c_str());
	envelope.AddInt32 ("max_tokens",    static_cast<int32_t>(settings.maxTokens));

	if (envelope.Flatten(&file) != B_OK) {
		std::fprintf(stderr, "session: flatten failed for %s\n", filePath.c_str());
		return {};
	}

	// Write BFS attributes.
	BNode node(filePath.c_str());
	if (node.InitCheck() == B_OK) {
		const std::string shortTitle = title.size() > 80
		    ? title.substr(0, 77) + "\xE2\x80\xA6"  // …
		    : title;
		WriteStrAttr (node, kAttrTitle,    shortTitle);
		WriteStrAttr (node, kAttrModel,    model);
		WriteInt32Attr(node, kAttrTurns,   static_cast<int32_t>(turns));
		WriteInt64Attr(node, kAttrModified, static_cast<int64_t>(now));

		// Only write created time on new files.
		if (existingPath.empty())
			WriteInt64Attr(node, kAttrCreated, static_cast<int64_t>(now));
	}

	// Stamp MIME type so Tracker can identify the file.
	StampMime(filePath);

	return filePath;
#endif
}


// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

nlohmann::json Load(const std::string& path)
{
#ifndef __HAIKU__
	(void)path;
	return nlohmann::json::array();
#else
	BFile file(path.c_str(), B_READ_ONLY);
	if (file.InitCheck() != B_OK) return nlohmann::json::array();

	BMessage envelope;
	if (envelope.Unflatten(&file) != B_OK) return nlohmann::json::array();

	const char* json_str = nullptr;
	if (envelope.FindString("messages", &json_str) != B_OK || !json_str)
		return nlohmann::json::array();

	try {
		auto parsed = nlohmann::json::parse(json_str);
		if (parsed.is_array()) return parsed;
	} catch (...) {}

	return nlohmann::json::array();
#endif
}


// ---------------------------------------------------------------------------
// LoadSettings
// ---------------------------------------------------------------------------

bool LoadSettings(const std::string& path, SessionSettings& out)
{
#ifndef __HAIKU__
	(void)path; (void)out;
	return false;
#else
	BFile file(path.c_str(), B_READ_ONLY);
	if (file.InitCheck() != B_OK) return false;

	BMessage envelope;
	if (envelope.Unflatten(&file) != B_OK) return false;

	const char* s = nullptr;
	if (envelope.FindString("model", &s) == B_OK && s)         out.model        = s;
	if (envelope.FindString("system_prompt", &s) == B_OK && s) out.systemPrompt = s;
	if (envelope.FindString("working_dir", &s) == B_OK && s)   out.workingDir   = s;
	int32 mt = 0;
	if (envelope.FindInt32("max_tokens", &mt) == B_OK)         out.maxTokens    = mt;
	return true;
#endif
}


// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

std::vector<SessionInfo> List()
{
	std::vector<SessionInfo> result;
#ifdef __HAIKU__
	const std::string dir = paths::SessionsDir();
	BDirectory bdir(dir.c_str());
	if (bdir.InitCheck() != B_OK) return result;

	BEntry entry;
	while (bdir.GetNextEntry(&entry) == B_OK) {
		// Skip non-files and non-.session files.
		if (!entry.IsFile()) continue;
		char name[B_FILE_NAME_LENGTH];
		entry.GetName(name);
		const std::string sname(name);
		if (sname.size() < 8 ||
		    sname.substr(sname.size() - 8) != ".session") continue;

		BPath entryPath;
		entry.GetPath(&entryPath);

		BNode node(&entry);
		if (node.InitCheck() != B_OK) continue;

		SessionInfo info;
		info.path     = entryPath.Path();
		info.title    = ReadStrAttr (node, kAttrTitle);
		info.model    = ReadStrAttr (node, kAttrModel);
		info.turns    = ReadInt32Attr(node, kAttrTurns);
		info.created  = static_cast<time_t>(ReadInt64Attr(node, kAttrCreated));
		info.modified = static_cast<time_t>(ReadInt64Attr(node, kAttrModified));

		// Fall back to filename as title if attribute is missing.
		if (info.title.empty()) info.title = sname;

		result.push_back(std::move(info));
	}

	// Sort by modified time, most recent first.
	std::sort(result.begin(), result.end(),
	    [](const SessionInfo& a, const SessionInfo& b) {
	        return a.modified > b.modified;
	    });
#endif
	return result;
}


// ---------------------------------------------------------------------------
// EnsureIndexes
// ---------------------------------------------------------------------------

void EnsureIndexes()
{
#ifdef __HAIKU__
	// Get the volume that holds the sessions directory.
	BVolumeRoster roster;
	BVolume vol;
	roster.GetBootVolume(&vol);

	// fs_create_index is idempotent — returns B_FILE_EXISTS if the
	// index already exists, which we silently ignore.
	auto mkidx = [&](const char* name, uint32_t type) {
		fs_create_index(vol.Device(), name, type, 0);
	};

	mkidx(kAttrTitle,    B_STRING_TYPE);
	mkidx(kAttrModel,    B_STRING_TYPE);
	mkidx(kAttrTurns,    B_INT32_TYPE);
	mkidx(kAttrCreated,  B_INT64_TYPE);
	mkidx(kAttrModified, B_INT64_TYPE);
#endif
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

bool Delete(const std::string& path)
{
	if (path.empty()) return false;
	BEntry entry(path.c_str());
	if (entry.InitCheck() != B_OK) return false;
	if (!entry.Exists()) return true;   // already gone
	return entry.Remove() == B_OK;
}

// ---------------------------------------------------------------------------
// Rename
// ---------------------------------------------------------------------------

bool Rename(const std::string& path, const std::string& newTitle)
{
#ifndef __HAIKU__
	(void)path; (void)newTitle;
	return false;
#else
	if (path.empty()) return false;
	BNode node(path.c_str());
	if (node.InitCheck() != B_OK) return false;

	// The title is held in the claude:title BFS attribute (what List()
	// reads). Truncate to the same 80-char cap used by Save().
	const std::string shortTitle = newTitle.size() > 80
	    ? newTitle.substr(0, 77) + "\xE2\x80\xA6"  // …
	    : newTitle;
	WriteStrAttr(node, kAttrTitle, shortTitle);

	// Bump modified time so the renamed session keeps its place at the
	// top of the newest-first list.
	WriteInt64Attr(node, kAttrModified,
	               static_cast<int64_t>(std::time(nullptr)));
	return true;
#endif
}

} // namespace session