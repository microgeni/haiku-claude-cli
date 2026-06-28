#include "agents.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace agents {

namespace {

std::vector<Agent> g_agents;

std::string slurp(const std::string& path) {
	std::ifstream f(path);
	if (!f.is_open()) return {};
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

std::string trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
	return s.substr(a, b - a);
}

std::string unquote(const std::string& s) {
	if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
	                   || (s.front() == '\'' && s.back() == '\''))) {
		return s.substr(1, s.size() - 2);
	}
	return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
	if (suffix.size() > s.size()) return false;
	return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void split_frontmatter(const std::string& content,
                       std::string& frontmatter, std::string& body) {
	frontmatter.clear();
	body = content;
	size_t i = 0;
	if (content.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
	if (content.compare(i, 3, "---") != 0) return;
	const size_t afterMarker = i + 3;
	if (afterMarker < content.size()
	    && content[afterMarker] != '\n' && content[afterMarker] != '\r')
		return;
	const size_t fmStart = content.find('\n', afterMarker);
	if (fmStart == std::string::npos) return;
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
}

void parse_frontmatter(const std::string& fm, Agent& agent) {
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
		if (key == "name")             agent.name = val;
		else if (key == "description") agent.description = val;
		else if (key == "tools")       agent.tools = val;
		else if (key == "model")       agent.model = val;
		else if (key == "color")       agent.color = val;
	}
}

void load_agent_file(const std::string& path, const std::string& fileStem) {
	const std::string content = slurp(path);
	if (content.empty()) return;

	Agent agent;
	agent.name = fileStem;  // default; frontmatter overrides
	std::string fm, body;
	split_frontmatter(content, fm, body);
	parse_frontmatter(fm, agent);
	if (agent.name.empty()) agent.name = fileStem;
	agent.prompt = trim(body);
	if (agent.prompt.empty()) return;  // a subagent needs a system prompt

	for (auto& existing : g_agents) {
		if (existing.name == agent.name) {
			existing = std::move(agent);
			return;
		}
	}
	g_agents.push_back(std::move(agent));
}

void scan_dir(const std::string& root) {
	DIR* d = opendir(root.c_str());
	if (!d) return;
	const struct dirent* ent;
	while ((ent = readdir(d)) != nullptr) {
		const std::string name = ent->d_name;
		if (!ends_with(name, ".md")) continue;
		const std::string stem = name.substr(0, name.size() - 3);
		load_agent_file(root + "/" + name, stem);
	}
	closedir(d);
}

} // namespace

void Load(const std::string& userDir, const std::string& projectDir) {
	g_agents.clear();
	scan_dir(userDir);
	scan_dir(projectDir);
	std::sort(g_agents.begin(), g_agents.end(),
		[](const Agent& a, const Agent& b) { return a.name < b.name; });
}

const std::vector<Agent>& All() { return g_agents; }

std::vector<std::string> Names() {
	std::vector<std::string> out;
	out.reserve(g_agents.size());
	for (const auto& a : g_agents) out.push_back(a.name);
	return out;
}

const Agent* Find(const std::string& name) {
	for (const auto& a : g_agents)
		if (a.name == name) return &a;
	return nullptr;
}

std::string ResolveModel(const Agent& a, const std::string& parentModel) {
	if (a.model.empty()) return parentModel;
	// Map the common short aliases onto current model ids. A full id is
	// passed through unchanged.
	std::string m = a.model;
	std::string lower = m;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lower == "haiku")  return "claude-haiku-4-5";
	if (lower == "sonnet") return "claude-sonnet-4-5";
	if (lower == "opus")   return "claude-opus-4-1";
	if (lower == "inherit") return parentModel;
	return m;
}

std::vector<std::string> ToolAllowList(const Agent& a) {
	std::vector<std::string> out;
	std::string token;
	for (char c : a.tools) {
		if (c == ',' || c == ' ' || c == '\t') {
			if (!token.empty()) { out.push_back(token); token.clear(); }
		} else {
			token += c;
		}
	}
	if (!token.empty()) out.push_back(token);
	return out;
}

std::string SystemBlock() {
	if (g_agents.empty()) return {};
	std::string block =
		"Available subagents (delegate via the Task tool by setting "
		"subagent_type to the matching name when a task fits its "
		"description):\n";
	for (const auto& a : g_agents) {
		block += "- " + a.name;
		if (!a.description.empty()) block += ": " + a.description;
		block += "\n";
	}
	return block;
}

} // namespace agents
