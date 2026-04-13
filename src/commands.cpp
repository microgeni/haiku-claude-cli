#include "commands.h"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

namespace commands {

namespace {

std::unordered_map<std::string, std::string> g_commands;

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool ends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void load_dir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const std::string name = ent->d_name;
        if (!ends_with(name, ".md")) continue;
        const std::string cmd_name = name.substr(0, name.size() - 3);
        std::string body = slurp(dir + "/" + name);
        // Trim trailing newline so substituted prompts stay tidy.
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
            body.pop_back();
        }
        if (body.empty()) continue;
        g_commands[cmd_name] = std::move(body);
    }
    closedir(d);
}

std::string substitute(const std::string& body, const std::string& args) {
    std::string out;
    out.reserve(body.size() + args.size());
    const std::string marker = "{{args}}";
    size_t i = 0;
    while (i < body.size()) {
        if (i + marker.size() <= body.size()
            && body.compare(i, marker.size(), marker) == 0) {
            out += args;
            i += marker.size();
        } else {
            out += body[i++];
        }
    }
    return out;
}

} // namespace

void load(const std::string& user_dir) {
    g_commands.clear();
    // User directory first; project definitions overwrite on collision.
    load_dir(user_dir);
    load_dir(".claude/commands");
}

std::vector<std::string> names() {
    std::vector<std::string> out;
    out.reserve(g_commands.size());
    for (const auto& entry : g_commands) out.push_back(entry.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> expand(const std::string& name, const std::string& args) {
    const auto it = g_commands.find(name);
    if (it == g_commands.end()) return std::nullopt;
    return substitute(it->second, args);
}

} // namespace commands
