#ifndef GUI_CODE_STYLER_H
#define GUI_CODE_STYLER_H

#include <map>
#include <string>
#include <vector>

// Code-block syntax highlighting for the GUI, driven by Genio's YAML theme +
// language schema so code renders in the SAME theme as the user's Genio editor.
//
// Engine: Scintilla + Lexilla (pkgman: lexilla_devel). Both Genio and Koder
// sit on this; we embed the engine directly rather than reusing either app.
// Theme/language files are MIT-licensed (Konradsson themes) -- license-clean
// to read and bundle. The user's own Genio theme is already in this format.
//
// Schema (confirmed against data/styles/dark.yaml + data/languages/c.yaml):
//
//   THEME (styles/<name>.yaml): named style -> { id, foreground, background, style[] }
//     Global:                       reserved Scintilla styles
//       Default:      { id: 32, ... }   -> STYLE_DEFAULT
//       Line number:  { id: 33, ... }   -> STYLE_LINENUMBER
//       Brace highlight: { id: 34 }     -> STYLE_BRACELIGHT  (etc.)
//     Keyword:  { id: 105, foreground: "#66BFFF", style: [bold] }
//     String:   { id: 106, ... }   Comment: { id: 101, ... }   ...
//   The `id` is Genio's CANONICAL style number; the theme speaks canonical ids.
//
//   LANGUAGE (languages/<name>.yaml):
//     lexer: cpp                       -> Lexilla lexer name (SCI_SETILEXER)
//     keywords: { 0: "...", 1: "..." } -> SCI_SETKEYWORDS per group
//     styles:   { <scintilla_style_num>: <canonical_id> }  -- the translation
//       e.g. cpp lexer style 5 -> 105 (keyword), 6/7 -> 106 (string), 1/2 -> 101
//
//   APPLY = for each (scintilla_style_num -> canonical_id) in the language file,
//   look up the color in the theme by canonical_id, emit SCI_STYLESETFORE/BACK/
//   BOLD/ITALIC/UNDERLINE on scintilla_style_num. That indirection is how one
//   theme colors every language.

namespace styling {

struct ThemeStyle {
    std::string foreground;     // "#RRGGBB", empty if unset
    std::string background;     // "#RRGGBB", empty if unset
    bool bold = false;
    bool italic = false;
    bool underline = false;
};

// Parsed theme: canonical_id -> style. Includes the Global reserved-id entries.
class Theme {
public:
    // Load a Genio styles/<name>.yaml. Returns false on parse failure.
    bool LoadFile(const std::string& path);
    const ThemeStyle* ByCanonicalId(int id) const;

private:
    std::map<int, ThemeStyle> fStyles;   // canonical id -> style
};

// Parsed language definition.
struct Language {
    std::string lexer;                          // Lexilla lexer name
    std::map<int, std::string> keywords;        // group index -> space-joined list
    std::map<int, int> styleMap;                // scintilla style num -> canonical id
};

class LanguageSet {
public:
    // Load all languages/*.yaml from a directory.
    bool LoadDir(const std::string& dir);
    // Resolve a fenced-code-block info string ("cpp", "python", "sh") to a
    // Language, with alias handling (c++/cpp, sh/bash, js/javascript...).
    const Language* Resolve(const std::string& fenceTag) const;

private:
    std::map<std::string, Language> fLangs;     // canonical lang name -> def
};

// Configures an already-created Scintilla view (passed as an opaque handle that
// can SendMessage SCI_* codes) for a given language using a given theme. This is
// the apply loop: lexer + keywords + per-style colors + the Global block.
//
// `sciSend` is a thin shim over Scintilla's BView SendMessage so this module
// does not depend on the concrete Scintilla-for-Haiku headers at interface
// level (keeps the dependency in the .cpp).
class CodeStyler {
public:
    CodeStyler(const Theme& theme, const LanguageSet& langs)
        : fTheme(theme), fLangs(langs) {}

    using SciSend = std::function<long(unsigned int msg, unsigned long w, long l)>;

    // Apply lexer + keywords + theme styling to the view. fenceTag selects the
    // language; unknown tags fall back to plain STYLE_DEFAULT only. Returns the
    // resolved lexer name (empty if none -> render as plain monospace).
    std::string Apply(const SciSend& sciSend, const std::string& fenceTag) const;

private:
    void ApplyGlobal(const SciSend& sciSend) const;  // STYLE_DEFAULT/LINENUMBER/...

    const Theme&       fTheme;
    const LanguageSet& fLangs;
};

} // namespace styling

#endif
