#ifndef HAIKU_CLAUDE_CLI_CODE_STYLER_H
#define HAIKU_CLAUDE_CLI_CODE_STYLER_H

#include <functional>
#include <map>
#include <string>
#include <vector>

// Code-block syntax highlighting for the GUI, driven by Genio's YAML theme +
// language schema so code renders in the SAME theme as the user's Genio editor.
//
// Engine: BScintillaView (libscintilla.so) + Lexilla (liblexilla.so).
// Both ship as Haiku packages; we use them directly via raw SCI_* messages.
//
// Schema (confirmed against gui/styling/reference/*.yaml):
//
//   THEME (styles/<name>.yaml):
//     Global: { Default: {id:32,...}, Line number: {id:33,...}, ... }
//     Keyword: { id: 105, foreground: "#66BFFF", style: [bold] }
//     The `id` is Genio's CANONICAL style number; the theme speaks these.
//
//   LANGUAGE (languages/<name>.yaml):
//     lexer: cpp                              -> Lexilla lexer name
//     keywords: { 0: "...", 1: "..." }        -> per-group keyword lists
//     styles: { <scintilla_num>: <canon_id> } -> translation table
//
//   APPLY: for each (scintilla_num → canon_id) in language.styles,
//     look up the ThemeStyle by canon_id, emit SCI_STYLESETFORE/BACK/
//     BOLD/ITALIC/UNDERLINE on scintilla_num.

namespace styling {

struct ThemeStyle {
	std::string foreground; // "#RRGGBB", empty if unset
	std::string background; // "#RRGGBB", empty if unset
	bool bold      = false;
	bool italic    = false;
	bool underline = false;
};

// Parsed theme: canonical_id → style.
class Theme {
public:
	// Load a Genio styles/<name>.yaml. Returns false on parse failure.
	bool LoadFile(const std::string& path);

	// Look up by canonical id. Returns nullptr if not found.
	const ThemeStyle* ByCanonicalId(int id) const;

	bool IsLoaded() const { return !fStyles.empty(); }

private:
	std::map<int, ThemeStyle> fStyles; // canonical id → style
};

// Parsed language definition.
struct Language {
	std::string lexer;                   // Lexilla lexer name ("cpp", "python"…)
	std::map<int, std::string> keywords; // group index → space-joined list
	std::map<int, int> styleMap;         // scintilla style num → canonical id
};

class LanguageSet {
public:
	// Load all *.yaml files from a languages/ directory. Returns false if
	// the directory doesn't exist or contains no parseable files.
	bool LoadDir(const std::string& dir);

	// Resolve a fenced-code-block info string ("cpp", "c++", "python",
	// "sh", "bash", "json"…) to a Language, with alias handling.
	// Returns nullptr if the language is unknown.
	const Language* Resolve(const std::string& fenceTag) const;

	bool IsLoaded() const { return !fLangs.empty(); }

private:
	// Register a language under its canonical name and common aliases.
	void Register(const std::string& canonName, Language lang);

	std::map<std::string, Language> fLangs; // canonical name → definition
	std::map<std::string, std::string> fAliases; // alias → canonical name
};

// Thin shim over BScintillaView::SendMessage so CodeStyler doesn't need to
// include the Scintilla headers at interface level.
using SciSend = std::function<long(unsigned int msg, unsigned long wParam, long lParam)>;

// Applies lexer + keywords + theme colours to an already-created Scintilla
// view. Call once after the view is constructed and before text is inserted.
//
// fenceTag selects the language (e.g. "cpp", "python"). Unknown tags apply
// only STYLE_DEFAULT colouring (monospace fallback). Returns the resolved
// Lexilla lexer name, or empty string if none matched.
class CodeStyler {
public:
	CodeStyler(const Theme& theme, const LanguageSet& langs)
		: fTheme(theme), fLangs(langs) {}

	std::string Apply(const SciSend& sci, const std::string& fenceTag) const;

	// Parse "#RRGGBB" → Scintilla's packed 0x00BBGGRR.
	static long ParseColor(const std::string& hex);

private:
	// Apply the Global block (STYLE_DEFAULT, STYLE_LINENUMBER, caret…).
	void ApplyGlobal(const SciSend& sci) const;

	const Theme&       fTheme;
	const LanguageSet& fLangs;
};

// Locate the default Genio theme file. Searches:
//   ~/config/settings/Genio/styles/dark.yaml   (user's live theme)
//   <app_dir>/../data/claude-gui/styles/dark.yaml (bundled fallback)
// Returns empty string if neither exists.
std::string FindDefaultTheme();

// Locate a languages/ directory. Searches the same two locations.
std::string FindLanguagesDir();

} // namespace styling

#endif // HAIKU_CLAUDE_CLI_CODE_STYLER_H
