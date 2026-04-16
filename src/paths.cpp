#include "paths.h"

#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>

namespace paths {

std::string config_dir() {
#ifdef __HAIKU__
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/boot/home") + "/config/settings/claude-cli";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/claude-cli";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/claude-cli";
#endif
}

std::string config_path()         { return config_dir() + "/config.json"; }
std::string history_path()        { return config_dir() + "/history.json"; }
std::string repl_history_path()   { return config_dir() + "/repl_history"; }
std::string log_dir()             { return config_dir() + "/logs"; }
std::string user_memory_path()    { return config_dir() + "/CLAUDE.md"; }
std::string project_memory_path() { return "CLAUDE.md"; }

// Walk `path` one component at a time and mkdir each prefix. mkdir(2)
// returning EEXIST is fine — someone (maybe a previous call, maybe the
// user) already created it. Any other error aborts the walk.
bool mkdir_p(const std::string& path) {
    std::string accum;
    for (size_t i = 0; i < path.size(); ++i) {
        accum += path[i];
        const bool boundary = (path[i] == '/') || (i + 1 == path.size());
        if (!boundary) continue;
        if (accum.empty() || accum == "/") continue;
        if (mkdir(accum.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}

} // namespace paths
