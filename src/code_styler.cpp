#include "code_styler.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __HAIKU__
#include <FindDirectory.h>
#include <Path.h>
#endif

#include <yaml-cpp/yaml.h>

// Scintilla SCI_* message numbers. We use the raw integers so we don't
// need to pull in Scintilla.h (which requires the full SDK).
// Values from Scintilla/include/Scintilla.h (stable ABI).
namespace sci {
constexpr unsigned int SETILEXER       = 4033;
constexpr unsigned int SETKEYWORDS     = 2005;
constexpr unsigned int STYLESETFORE    = 2051;
constexpr unsigned int STYLESETBACK    = 2052;
constexpr unsigned int STYLESETBOLD    = 2053;
constexpr unsigned int STYLESETITALIC  = 2056;
constexpr unsigned int STYLESETUNDERLINE = 2059;
constexpr unsigned int STYLESETSIZE    = 2055;
constexpr unsigned int STYLERESETDEFAULT = 2058;
constexpr unsigned int STYLECLEARALL   = 2050;
constexpr unsigned int SETLEXERLANGUAGE = 4006;
// Predefined style numbers
constexpr int STYLE_DEFAULT    = 32;
constexpr int STYLE_LINENUMBER = 33;
constexpr int STYLE_BRACELIGHT = 34;
constexpr int STYLE_BRACEBAD   = 35;
constexpr int STYLE_INDENTGUIDE = 37;
constexpr unsigned int SETCARETFORE = 2069;
constexpr unsigned int SETSELBACK   = 2068;
} // namespace sci

namespace styling {

// ---------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------

// Parse "#RRGGBB" → Scintilla's packed 0x00BBGGRR (little-endian RGB).
long CodeStyler::ParseColor(const std::string& hex)
{
	if (hex.size() < 7 || hex[0] != '#') return 0;
	const auto h2 = [&](int pos) -> long {
		const char hi = hex[pos];
		const char lo = hex[pos + 1];
		auto c2i = [](char c) -> long {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return 0;
		};
		return (c2i(hi) << 4) | c2i(lo);
	};
	const long r = h2(1), g = h2(3), b = h2(5);
	return r | (g << 8) | (b << 16); // Scintilla BGR packed
}

// ---------------------------------------------------------------------------
// Theme::LoadFile  (parses Genio styles/<name>.yaml)
// ---------------------------------------------------------------------------

static void ParseStyleNode(const YAML::Node& node, int canon_id,
                            std::map<int, ThemeStyle>& out)
{
	if (!node.IsMap()) return;
	ThemeStyle s;
	if (node["foreground"]) s.foreground = node["foreground"].as<std::string>();
	if (node["background"]) s.background = node["background"].as<std::string>();
	if (node["style"] && node["style"].IsSequence()) {
		for (const auto& tag : node["style"]) {
			const std::string t = tag.as<std::string>();
			if (t == "bold")      s.bold      = true;
			if (t == "italic")    s.italic    = true;
			if (t == "underline") s.underline = true;
		}
	}
	out[canon_id] = std::move(s);
}

bool Theme::LoadFile(const std::string& path)
{
	// Same Flex-lexer guard as ParseLanguageFile: skip missing/empty files
	// so yaml-cpp never prints "input in flex scanner failed" to stderr.
	struct stat st;
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
		return false;

	try {
		YAML::Node root = YAML::LoadFile(path);
		if (!root.IsMap()) return false;

		for (const auto& entry : root) {
			const std::string key = entry.first.as<std::string>();
			const YAML::Node& val = entry.second;
			if (!val.IsMap()) continue;

			// Each top-level key is a style name. The Global: section
			// contains nested sub-keys, each with their own id.
			if (key == "Global") {
				for (const auto& sub : val) {
					const YAML::Node& sv = sub.second;
					if (!sv.IsMap()) continue;
					if (sv["id"]) {
						const int id = sv["id"].as<int>();
						ParseStyleNode(sv, id, fStyles);
					}
				}
			} else if (val["id"]) {
				const int id = val["id"].as<int>();
				ParseStyleNode(val, id, fStyles);
			}
		}
		return !fStyles.empty();
	} catch (const YAML::Exception&) {
		return false;
	}
}

const ThemeStyle* Theme::ByCanonicalId(int id) const
{
	auto it = fStyles.find(id);
	return (it != fStyles.end()) ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------
// LanguageSet::LoadDir + Resolve
// ---------------------------------------------------------------------------

static std::string ToLower(std::string s)
{
	for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

static bool ParseLanguageFile(const std::string& path, Language& out)
{
	// Never hand a missing or empty file to YAML::LoadFile: yaml-cpp's Flex
	// lexer prints "input in flex scanner failed" to stderr on a bad stream
	// before the C++ exception is thrown, so guard with a stat() first.
	struct stat st;
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
		return false;

	try {
		YAML::Node root = YAML::LoadFile(path);
		if (!root.IsMap()) return false;

		if (root["lexer"]) out.lexer = root["lexer"].as<std::string>();
		if (out.lexer.empty()) return false;

		if (root["keywords"] && root["keywords"].IsMap()) {
			for (const auto& kv : root["keywords"]) {
				int group = kv.first.as<int>();
				out.keywords[group] = kv.second.as<std::string>();
			}
		}
		if (root["styles"] && root["styles"].IsMap()) {
			for (const auto& kv : root["styles"]) {
				int scinum = kv.first.as<int>();
				int canon  = kv.second.as<int>();
				out.styleMap[scinum] = canon;
			}
		}
		return true;
	} catch (const YAML::Exception&) {
		return false;
	}
}

void LanguageSet::Register(const std::string& canonName, Language lang)
{
	fLangs[canonName] = std::move(lang);
}

bool LanguageSet::LoadDir(const std::string& dir)
{
	// We can't use readdir without dragging in more headers; use a
	// fixed list of known language file names that match the reference
	// YAML files bundled with the app. Unknown files are simply skipped.
	// This is intentional — we support a curated set for Step 6 and
	// expand the list as more language files are bundled.
	const std::vector<std::pair<std::string, std::string>> known = {
		{ "c",       "c.yaml"       },
		{ "cpp",     "cpp.yaml"     },
		{ "python",  "python.yaml"  },
		{ "sh",      "sh.yaml"      },
		{ "bash",    "bash.yaml"    },
		{ "json",    "json.yaml"    },
		{ "make",    "makefile.yaml"},
		{ "cmake",   "cmake.yaml"   },
		{ "html",    "html.yaml"    },
		{ "xml",     "xml.yaml"     },
		{ "css",     "css.yaml"     },
		{ "diff",    "diff.yaml"    },
		{ "markdown","markdown.yaml"},
	};

	bool any = false;
	for (const auto& [name, file] : known) {
		const std::string full = dir + "/" + file;
		Language lang;
		if (ParseLanguageFile(full, lang)) {
			fLangs[name] = std::move(lang);
			any = true;
		}
	}

	// Register canonical aliases so fenced-code-block tags resolve.
	// (aliases point to the canonical name's entry via the Resolve map)
	auto alias = [&](const std::string& from, const std::string& to) {
		if (fLangs.count(to)) fAliases[from] = to;
	};
	alias("c++",        "cpp");
	alias("cxx",        "cpp");
	alias("cc",         "cpp");
	alias("h",          "cpp");
	alias("hpp",        "cpp");
	alias("shell",      "sh");
	alias("zsh",        "sh");
	alias("fish",       "sh");
	alias("makefile",   "make");
	alias("Makefile",   "make");
	alias("md",         "markdown");
	alias("js",         "json");   // close enough for highlighting
	alias("javascript", "json");
	alias("ts",         "json");
	alias("typescript", "json");

	return any;
}

const Language* LanguageSet::Resolve(const std::string& fenceTag) const
{
	const std::string tag = ToLower(fenceTag);
	auto it = fLangs.find(tag);
	if (it != fLangs.end()) return &it->second;
	auto al = fAliases.find(tag);
	if (al != fAliases.end()) {
		auto it2 = fLangs.find(al->second);
		if (it2 != fLangs.end()) return &it2->second;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// CodeStyler::Apply
// ---------------------------------------------------------------------------

void CodeStyler::ApplyGlobal(const SciSend& sci) const
{
	// STYLE_DEFAULT (32) — the base style everything inherits from.
	if (const ThemeStyle* def = fTheme.ByCanonicalId(sci::STYLE_DEFAULT)) {
		if (!def->foreground.empty())
			sci(sci::STYLESETFORE, sci::STYLE_DEFAULT, ParseColor(def->foreground));
		if (!def->background.empty())
			sci(sci::STYLESETBACK, sci::STYLE_DEFAULT, ParseColor(def->background));
	}

	// Propagate STYLE_DEFAULT to all styles, then let per-style overrides
	// override. STYLECLEARALL must come AFTER STYLESETFORE/BACK on default.
	sci(sci::STYLECLEARALL, 0, 0);

	// STYLE_LINENUMBER (33).
	if (const ThemeStyle* ln = fTheme.ByCanonicalId(sci::STYLE_LINENUMBER)) {
		if (!ln->foreground.empty())
			sci(sci::STYLESETFORE, sci::STYLE_LINENUMBER, ParseColor(ln->foreground));
		if (!ln->background.empty())
			sci(sci::STYLESETBACK, sci::STYLE_LINENUMBER, ParseColor(ln->background));
	}

	// Caret colour (canonical id -1 special case, stored under "Caret" in
	// the Global block without an id field — use SCI_SETCARETFORE directly).
	// In the dark theme the caret is "#FF4050".
	// Since we have no canonical id for it, look it up by iterating the
	// theme file's known caret colour (#FF4050) as a fallback.
	sci(sci::SETCARETFORE, 0, ParseColor("#FF4050")); // hardcoded dark theme caret

	// Selection background.
	sci(sci::SETSELBACK, 1, ParseColor("#474247")); // dark theme selected bg
}

std::string CodeStyler::Apply(const SciSend& sci, const std::string& fenceTag) const
{
	// Step 1: apply global defaults.
	ApplyGlobal(sci);

	// Step 2: resolve the language.
	const Language* lang = fLangs.Resolve(fenceTag);
	if (!lang) {
		// Unknown language — plain monospace (STYLE_DEFAULT already set).
		return {};
	}

	// Step 3: set the Lexilla lexer via SCI_SETLEXERLANGUAGE.
	// SCI_SETILEXER(0, ILexer5*) is the modern API but requires the full
	// Scintilla headers. Use SCI_SETLEXERLANGUAGE (string API) instead —
	// it's stable and doesn't require Scintilla.h.
	sci(sci::SETLEXERLANGUAGE, 0, reinterpret_cast<long>(lang->lexer.c_str()));

	// Step 4: set keyword groups.
	for (const auto& [group, words] : lang->keywords) {
		sci(sci::SETKEYWORDS,
		    static_cast<unsigned long>(group),
		    reinterpret_cast<long>(words.c_str()));
	}

	// Step 5: apply per-style colours from the translation table.
	// styleMap[scintilla_num] → canon_id → ThemeStyle → SCI_STYLESET*
	for (const auto& [scinum, canonId] : lang->styleMap) {
		const ThemeStyle* ts = fTheme.ByCanonicalId(canonId);
		if (!ts) continue;
		if (!ts->foreground.empty())
			sci(sci::STYLESETFORE, static_cast<unsigned long>(scinum),
			    ParseColor(ts->foreground));
		if (!ts->background.empty())
			sci(sci::STYLESETBACK, static_cast<unsigned long>(scinum),
			    ParseColor(ts->background));
		if (ts->bold)
			sci(sci::STYLESETBOLD,      static_cast<unsigned long>(scinum), 1);
		if (ts->italic)
			sci(sci::STYLESETITALIC,    static_cast<unsigned long>(scinum), 1);
		if (ts->underline)
			sci(sci::STYLESETUNDERLINE, static_cast<unsigned long>(scinum), 1);
	}

	return lang->lexer;
}

// ---------------------------------------------------------------------------
// FindDefaultTheme / FindLanguagesDir
// ---------------------------------------------------------------------------

std::string FindDefaultTheme()
{
#ifdef __HAIKU__
	// 1. User's live Genio theme.
	BPath settings;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings) == B_OK) {
		std::string p = std::string(settings.Path()) + "/Genio/styles/dark.yaml";
		struct stat st;
		if (stat(p.c_str(), &st) == 0) return p;
	}

	// 2. Installed app data (make install puts it here).
	BPath nonpkg;
	if (find_directory(B_SYSTEM_NONPACKAGED_DATA_DIRECTORY, &nonpkg) == B_OK) {
		std::string p = std::string(nonpkg.Path()) + "/claude-gui/styles/dark.yaml";
		struct stat st;
		if (stat(p.c_str(), &st) == 0) return p;
	}

	// 3. Development fallback: assets/data/styles/dark.yaml next to the binary.
	char exe[PATH_MAX] {};
	if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) > 0) {  // flawfinder: ignore (own binary path; buffer zero-initialized, return checked)
		std::string dir(exe);
		auto sl = dir.rfind('/');
		if (sl != std::string::npos) dir.resize(sl);
		// Walk up to project root (binary is in build/).
		std::string p = dir + "/../assets/data/styles/dark.yaml";
		struct stat st;
		if (stat(p.c_str(), &st) == 0) return p;
	}
#endif
	return {};
}

std::string FindLanguagesDir()
{
#ifdef __HAIKU__
	// A languages directory only counts if it actually contains a known
	// grammar file. Genio creates an empty ~/config/settings/Genio/languages
	// directory; accepting it caused code_styler to try opening files that
	// don't exist, which makes yaml-cpp's Flex lexer print "input in flex
	// scanner failed" to stderr. Probe for cpp.yaml as a sentinel and fall
	// through to the next candidate when the directory is empty.
	auto hasGrammars = [](const std::string& dir) {
		struct stat st;
		const std::string sentinel = dir + "/cpp.yaml";
		return stat(sentinel.c_str(), &st) == 0 && S_ISREG(st.st_mode);
	};

	BPath settings;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings) == B_OK) {
		std::string p = std::string(settings.Path()) + "/Genio/languages";
		if (hasGrammars(p)) return p;
	}

	BPath nonpkg;
	if (find_directory(B_SYSTEM_NONPACKAGED_DATA_DIRECTORY, &nonpkg) == B_OK) {
		std::string p = std::string(nonpkg.Path()) + "/claude-gui/languages";
		if (hasGrammars(p)) return p;
	}

	// Development fallback.
	char exe[PATH_MAX] {};
	if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) > 0) {  // flawfinder: ignore (own binary path; buffer zero-initialized, return checked)
		std::string dir(exe);
		auto sl = dir.rfind('/');
		if (sl != std::string::npos) dir.resize(sl);
		std::string p = dir + "/../assets/data/languages";
		if (hasGrammars(p)) return p;
	}
#endif
	return {};
}

} // namespace styling
