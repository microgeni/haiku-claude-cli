#include "skills.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "paths.h"

namespace skills {

namespace {

std::vector<Skill> g_skills;

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
std::string inject_dynamic_context(const std::string& body) {
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
				out += capture_command(cmd);
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

std::string Expand(const std::string& name, const std::string& args, bool& found) {
	const Skill* s = Find(name);
	if (!s) { found = false; return {}; }
	found = true;
	std::string expanded = substitute_args(s->body, args);
	expanded = inject_dynamic_context(expanded);
	return trim(expanded);
}

std::string SystemBlock() {
	std::string block;
	for (const auto& s : g_skills) {
		if (s.disableModelInvocation) continue;
		if (s.description.empty()) continue;
		block += "- " + s.name + ": " + s.description + "\n";
	}
	if (block.empty()) return {};
	return "Available skills (invoke by following the instructions for "
	       "the matching one when a user request fits its description; the "
	       "user can also invoke a skill directly by typing /skill-name):\n"
	     + block;
}

} // namespace skills
