#include "skills.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifdef __HAIKU__
#include <Node.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <fs_attr.h>
#include <fs_index.h>
#endif

#include "paths.h"

namespace skills {

namespace {

std::vector<Skill> g_skills;

// BFS attribute names for usage telemetry. Kept in the claude:
// namespace so they never collide with BEOS:* or user attributes.
const char* const kAttrUses     = "claude:skill_uses";
const char* const kAttrLastUsed = "claude:skill_lastused";
const char* const kAttrState    = "claude:skill_state";
const char* const kAttrPinned   = "claude:skill_pinned";

const char* const kStateActive   = "active";
const char* const kStateStale    = "stale";
const char* const kStateArchived = "archived";

// Path to the SKILL.md that carries a skill's attributes.
std::string skill_file(const Skill& s) { return s.dir + "/SKILL.md"; }

#ifdef __HAIKU__

// Read the usage attributes off a skill's SKILL.md. Missing attributes
// leave the corresponding fields at their defaults, so a skill that has
// never been invoked simply reads as zero uses / never used.
void read_usage_attrs(Skill& s) {
	BNode node(skill_file(s).c_str());
	if (node.InitCheck() != B_OK) return;

	int32 uses = 0;
	if (node.ReadAttr(kAttrUses, B_INT32_TYPE, 0, &uses, sizeof(uses))
			== static_cast<ssize_t>(sizeof(uses)))
		s.uses = uses;

	int64 last = 0;
	if (node.ReadAttr(kAttrLastUsed, B_INT64_TYPE, 0, &last, sizeof(last))
			== static_cast<ssize_t>(sizeof(last)))
		s.lastUsed = static_cast<time_t>(last);

	attr_info info {};
	if (node.GetAttrInfo(kAttrState, &info) == B_OK && info.size > 0) {
		std::string val(static_cast<size_t>(info.size), '\0');
		if (node.ReadAttr(kAttrState, B_STRING_TYPE, 0, &val[0],
				static_cast<size_t>(info.size)) > 0) {
			// Stored with a trailing NUL; trim it.
			const size_t nul = val.find('\0');
			if (nul != std::string::npos) val.resize(nul);
			s.state = val;
		}
	}

	bool pinned = false;
	if (node.ReadAttr(kAttrPinned, B_BOOL_TYPE, 0, &pinned, sizeof(pinned))
			== static_cast<ssize_t>(sizeof(pinned)))
		s.pinned = pinned;
}

// Write the state attribute back to disk.
void write_state_attr(const Skill& s, const char* state) {
	BNode node(skill_file(s).c_str());
	if (node.InitCheck() != B_OK) return;
	node.WriteAttr(kAttrState, B_STRING_TYPE, 0, state, std::strlen(state) + 1);
}

#else  // !__HAIKU__

void read_usage_attrs(Skill&) {}
void write_state_attr(const Skill&, const char*) {}

#endif

std::string slurp(const std::string& path) {
	std::ifstream f(path);
	if (!f.is_open()) return {};
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

bool is_dir(const std::string& path) {
	struct stat st {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Trim leading/trailing ASCII whitespace.
std::string trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
	return s.substr(a, b - a);
}

// Strip optional matching surrounding quotes from a YAML scalar.
std::string unquote(const std::string& s) {
	if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
	                   || (s.front() == '\'' && s.back() == '\''))) {
		return s.substr(1, s.size() - 2);
	}
	return s;
}

bool truthy(const std::string& v) {
	std::string s = v;
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s == "true" || s == "yes" || s == "1" || s == "on";
}

// Split a SKILL.md file into (frontmatter, body). When the file opens
// with a `---` line, everything up to the next `---` line is treated as
// YAML frontmatter and the remainder is the body. Otherwise the whole
// file is the body and frontmatter is empty.
void split_frontmatter(const std::string& content,
                       std::string& frontmatter, std::string& body) {
	frontmatter.clear();
	body = content;
	// Must start with --- on the very first line.
	size_t i = 0;
	// Skip a leading UTF-8 BOM if present.
	if (content.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
	if (content.compare(i, 3, "---") != 0) return;
	const size_t afterMarker = i + 3;
	// Require the marker to be its own line.
	if (afterMarker < content.size()
	    && content[afterMarker] != '\n' && content[afterMarker] != '\r')
		return;
	const size_t fmStart = content.find('\n', afterMarker);
	if (fmStart == std::string::npos) return;
	// Find the closing --- on its own line.
	size_t pos = fmStart + 1;
	while (pos < content.size()) {
		size_t eol = content.find('\n', pos);
		const std::string line = content.substr(pos,
			eol == std::string::npos ? std::string::npos : eol - pos);
		if (trim(line) == "---") {
			frontmatter = content.substr(fmStart + 1, pos - (fmStart + 1));
			body = (eol == std::string::npos) ? std::string{}
			                                  : content.substr(eol + 1);
			return;
		}
		if (eol == std::string::npos) break;
		pos = eol + 1;
	}
	// No closing marker — treat the whole thing as body.
}

// Parse the handful of frontmatter keys we care about. This is a
// deliberately small flat key: value parser (not a full YAML engine).
void parse_frontmatter(const std::string& fm, Skill& skill) {
	std::istringstream iss(fm);
	std::string line;
	while (std::getline(iss, line)) {
		const std::string t = trim(line);
		if (t.empty() || t[0] == '#') continue;
		const size_t colon = t.find(':');
		if (colon == std::string::npos) continue;
		std::string key = trim(t.substr(0, colon));
		std::string val = unquote(trim(t.substr(colon + 1)));
		std::transform(key.begin(), key.end(), key.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (key == "name")                          skill.name = val;
		else if (key == "description")              skill.description = val;
		else if (key == "allowed-tools")            skill.allowedTools = val;
		else if (key == "disable-model-invocation") skill.disableModelInvocation = truthy(val);
	}
}

// Run a shell command and capture stdout (used for !`cmd` injection).
// Errors collapse to a short note rather than aborting the expansion.
std::string capture_command(const std::string& cmd) {
	std::array<char, 4096> buf {};
	std::string out;
	FILE* pipe = ::popen(cmd.c_str(), "r");  // flawfinder: ignore
	if (!pipe) return "[skill: command failed to start: " + cmd + "]";
	size_t total = 0;
	constexpr size_t kMax = 64 * 1024;
	while (size_t n = std::fread(buf.data(), 1, buf.size(), pipe)) {
		out.append(buf.data(), n);
		total += n;
		if (total >= kMax) { out += "\n[... truncated]"; break; }
	}
	::pclose(pipe);
	// Trim a single trailing newline for tidy inlining.
	if (!out.empty() && out.back() == '\n') out.pop_back();
	return out;
}

// Replace every {{args}} marker in `body` with `args`.
std::string substitute_args(const std::string& body, const std::string& args) {
	const std::string marker = "{{args}}";
	std::string out;
	out.reserve(body.size() + args.size());
	size_t i = 0;
	while (i < body.size()) {
		if (body.compare(i, marker.size(), marker) == 0) {
			out += args;
			i += marker.size();
		} else {
			out += body[i++];
		}
	}
	return out;
}

// Replace dynamic-context lines of the form  !`shell command`  with the
// command's stdout. The marker is a backtick-delimited command preceded
// by `!`. Multiple may appear; each is expanded independently.
std::string inject_dynamic_context(const std::string& body, bool runShell) {
	std::string out;
	out.reserve(body.size());
	size_t i = 0;
	while (i < body.size()) {
		// Look for the literal sequence: ! `
		if (body[i] == '!' && i + 1 < body.size() && body[i + 1] == '`') {
			const size_t cmdStart = i + 2;
			const size_t close = body.find('`', cmdStart);
			if (close != std::string::npos) {
				const std::string cmd = body.substr(cmdStart, close - cmdStart);
				if (runShell) {
					out += capture_command(cmd);
				} else {
					// Plan mode / read-only: show what would have run
					// rather than running it.
					out += "[not run in plan mode: " + cmd + "]";
				}
				i = close + 1;
				continue;
			}
		}
		out += body[i++];
	}
	return out;
}

// Load one skill directory. Returns true when a SKILL.md was found
// and registered (or replaced an existing same-named entry).
bool load_skill_dir(const std::string& dir, const std::string& dirName) {
	const std::string skillFile = dir + "/SKILL.md";
	const std::string content = slurp(skillFile);
	if (content.empty()) return false;

	Skill skill;
	skill.dir  = dir;
	skill.name = dirName;  // default; frontmatter `name` overrides
	std::string fm, body;
	split_frontmatter(content, fm, body);
	parse_frontmatter(fm, skill);
	if (skill.name.empty()) skill.name = dirName;
	skill.body = body;
	// If the description is empty, fall back to the first non-blank
	// body paragraph so the model still has a hint.
	if (skill.description.empty()) {
		std::istringstream iss(body);
		std::string line;
		while (std::getline(iss, line)) {
			const std::string t = trim(line);
			if (!t.empty() && t[0] != '#') { skill.description = t; break; }
		}
	}

	// Pull in usage telemetry so callers (listings, SystemBlock) can
	// see counts and lifecycle state without a second pass.
	read_usage_attrs(skill);

	// Replace any existing entry with the same name (project overrides
	// user because the project dir is scanned second).
	for (auto& existing : g_skills) {
		if (existing.name == skill.name) {
			existing = std::move(skill);
			return true;
		}
	}
	g_skills.push_back(std::move(skill));
	return true;
}

void scan_dir(const std::string& root) {
	DIR* d = opendir(root.c_str());
	if (!d) return;
	const struct dirent* ent;
	while ((ent = readdir(d)) != nullptr) {
		const std::string name = ent->d_name;
		if (name == "." || name == "..") continue;
		const std::string sub = root + "/" + name;
		if (is_dir(sub)) load_skill_dir(sub, name);
	}
	closedir(d);
}

} // namespace

void Load(const std::string& userDir, const std::string& projectDir) {
	g_skills.clear();
	// User first, project second so project entries override.
	scan_dir(userDir);
	scan_dir(projectDir);
	std::sort(g_skills.begin(), g_skills.end(),
		[](const Skill& a, const Skill& b) { return a.name < b.name; });
}

const std::vector<Skill>& All() { return g_skills; }

std::vector<std::string> Names() {
	std::vector<std::string> out;
	out.reserve(g_skills.size());
	for (const auto& s : g_skills) out.push_back(s.name);
	return out;
}

const Skill* Find(const std::string& name) {
	for (const auto& s : g_skills)
		if (s.name == name) return &s;
	return nullptr;
}

bool BodyRunsShell(const std::string& name) {
	const Skill* s = Find(name);
	if (!s) return false;
	// Mirror inject_dynamic_context's scan: a '!' immediately followed by
	// a backtick, with a closing backtick somewhere after it.
	const std::string& b = s->body;
	for (size_t i = 0; i + 1 < b.size(); ++i) {
		if (b[i] == '!' && b[i + 1] == '`'
		    && b.find('`', i + 2) != std::string::npos)
			return true;
	}
	return false;
}

std::string Expand(const std::string& name, const std::string& args, bool& found,
                   bool runShell) {
	const Skill* s = Find(name);
	if (!s) { found = false; return {}; }
	found = true;
	// Telemetry before expansion: an invocation counts even if a
	// !`cmd` line inside the body later fails.
	RecordUse(name);
	std::string expanded = substitute_args(s->body, args);
	expanded = inject_dynamic_context(expanded, runShell);
	return trim(expanded);
}

std::string SystemBlock() {
	std::string block;
	bool anyRunsShell = false;
	for (const auto& s : g_skills) {
		if (s.disableModelInvocation) continue;
		if (s.description.empty()) continue;
		// Archived skills stay on disk and stay invocable by name, but
		// are dropped from the system prompt — carrying a skill nobody
		// has used in months costs tokens on every single request.
		if (s.state == kStateArchived) continue;
		block += "- " + s.name + ": " + s.description + "\n";
		if (BodyRunsShell(s.name)) anyRunsShell = true;
	}

	std::string out;

	// ── Index of what exists (omitted when nothing is invocable) ──────
	if (!block.empty()) {
		out += "Available skills — reusable procedures for specific tasks:\n";
		out += block;
		out += "\nThese one-line descriptions are an index, NOT the procedure. "
			   "When a request matches one, call the `Skill` tool with that "
			   "name to load the actual steps, then follow them. Do not infer "
			   "a skill's contents from its description or improvise the "
			   "procedure yourself. The user can also run a skill directly by "
			   "typing /skill-name.\n";
		if (anyRunsShell) {
			// Be explicit that loading is not always free, so the model
			// doesn't treat Skill as a zero-consequence lookup.
			out += "Some skills run shell commands as part of loading; those "
				   "prompt for permission first.\n";
		}
		out += "\n";
	}

	// ── How new skills come into existence ────────────────────────────
	// Deliberately present even with zero skills installed: a fresh setup
	// is exactly when the model needs to know this is possible, otherwise
	// the loop never starts. Kept short because it sits in the cached
	// stable tier of every request.
	//
	// Suggest-only by design. Writing files into the user's config dir
	// mid-task is a surprise, and the point is to make the capability
	// discoverable, not to accumulate skills nobody asked for.
	out += "Creating skills: when you finish something non-trivial — a task "
		   "that took several tool calls, a fiddly fix, or a workflow worth "
		   "repeating — briefly offer to save it as a skill for next time. "
		   "Offer; do not create one unprompted, and do not offer for routine "
		   "one-step requests. If the user accepts, write "
		 + paths::UserSkillsDir() + "/<name>/SKILL.md (or "
		 + paths::ProjectSkillsDir() + "/<name>/SKILL.md when it is specific "
		   "to this project) with YAML frontmatter containing `name:` and a "
		   "`description:` of 60 characters or fewer — the description is "
		   "loaded into every future session, so keep it to one short "
		   "sentence — followed by the procedure in markdown. The user can "
		   "also run /learn themselves to have you do this in more depth.\n"
		   "If a skill you loaded turns out to be wrong, outdated, or missing "
		   "a step, say so and offer to correct it. Stale skills are worse "
		   "than none.\n";

	return out;
}

void RecordUse(const std::string& name) {
#ifdef __HAIKU__
	Skill* target = nullptr;
	for (auto& s : g_skills)
		if (s.name == name) { target = &s; break; }
	if (!target) return;

	BNode node(skill_file(*target).c_str());
	if (node.InitCheck() != B_OK) return;

	const int32 uses = target->uses + 1;
	const int64 now  = static_cast<int64>(::time(nullptr));
	node.WriteAttr(kAttrUses, B_INT32_TYPE, 0, &uses, sizeof(uses));
	node.WriteAttr(kAttrLastUsed, B_INT64_TYPE, 0, &now, sizeof(now));

	target->uses     = uses;
	target->lastUsed = static_cast<time_t>(now);

	// Using a skill revives it: a stale or archived skill that just
	// proved useful should not stay demoted until the next sweep.
	if (target->state == kStateStale || target->state == kStateArchived) {
		node.WriteAttr(kAttrState, B_STRING_TYPE, 0,
			kStateActive, std::strlen(kStateActive) + 1);
		target->state = kStateActive;
	}
#else
	(void)name;
#endif
}

int ApplyLifecycle(int staleAfterDays, int archiveAfterDays) {
#ifdef __HAIKU__
	if (staleAfterDays <= 0 || archiveAfterDays <= 0) return 0;

	const time_t now = ::time(nullptr);
	int changed = 0;

	for (auto& s : g_skills) {
		// Pinned skills opt out of all automatic transitions.
		if (s.pinned) continue;

		// A skill that has never been invoked ages from its file
		// mtime, not from epoch — otherwise every newly authored
		// skill would be archived the moment it is written.
		time_t reference = s.lastUsed;
		if (reference == 0) {
			struct stat st {};
			if (stat(skill_file(s).c_str(), &st) == 0) reference = st.st_mtime;
			else continue;
		}

		const double ageDays = std::difftime(now, reference) / 86400.0;
		const char*  want    = kStateActive;
		if (ageDays >= archiveAfterDays)   want = kStateArchived;
		else if (ageDays >= staleAfterDays) want = kStateStale;

		// Treat an unset state as "active" so a no-op sweep writes nothing.
		const std::string current = s.state.empty() ? kStateActive : s.state;
		if (current == want) continue;

		write_state_attr(s, want);
		s.state = want;
		++changed;
	}
	return changed;
#else
	(void)staleAfterDays; (void)archiveAfterDays;
	return 0;
#endif
}

bool SetPinned(const std::string& name, bool pinned) {
#ifdef __HAIKU__
	for (auto& s : g_skills) {
		if (s.name != name) continue;
		BNode node(skill_file(s).c_str());
		if (node.InitCheck() != B_OK) return false;
		node.WriteAttr(kAttrPinned, B_BOOL_TYPE, 0, &pinned, sizeof(pinned));
		s.pinned = pinned;
		// Pinning a demoted skill restores it immediately; the user is
		// saying it matters regardless of how long it has sat unused.
		if (pinned && (s.state == kStateStale || s.state == kStateArchived)) {
			write_state_attr(s, kStateActive);
			s.state = kStateActive;
		}
		return true;
	}
	return false;
#else
	(void)name; (void)pinned;
	return false;
#endif
}

void EnsureUsageIndexes() {
#ifdef __HAIKU__
	BVolume    vol;
	BVolumeRoster roster;
	if (roster.GetBootVolume(&vol) != B_OK) return;
	const dev_t dev = vol.Device();
	// fs_create_index returns an error when the index already exists;
	// that is the expected steady state, so the result is ignored.
	fs_create_index(dev, kAttrUses,     B_INT32_TYPE,  0);
	fs_create_index(dev, kAttrLastUsed, B_INT64_TYPE,  0);
	fs_create_index(dev, kAttrState,    B_STRING_TYPE, 0);
#endif
}

} // namespace skills
