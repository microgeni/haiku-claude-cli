#pragma once
// SciOutput — a BScintillaView configured as a read-only chat scrollback.
//
// Wraps a single BScintillaView that acts as the entire output area of the
// chat window. Text is appended in styled segments:
//
//   Style 0  (STYLE_DEFAULT)  — normal prose text
//   Style 1                   — user label  ("you ▸")
//   Style 2                   — model label ("claude ▸")
//   Style 3                   — tool/dim lines
//   Style 4                   — error text
//   Style 5                   — inline code / monospace
//   Style 6                   — heading H1
//   Style 7                   — heading H2
//   Style 8                   — heading H3
//   Style 9                   — link text
//   Style 10                  — blockquote
//   Style 11                  — dim/muted (list bullets, numbers)
//
// For fenced code blocks a Lexilla lexer is activated for just that byte
// range using SCI_STARTSTYLING + SCI_SETLEXERLANGUAGE.
//
// Threading: all public methods must be called from the window's looper
// thread (i.e., from MessageReceived or layout callbacks). They are NOT
// thread-safe — same contract as BTextView.

#include <string>

#include "scintilla_view.h"

// Forward declarations.
namespace styling {
	class Theme;
	class LanguageSet;
	class CodeStyler;
}

class SciOutput : public BScintillaView {
public:
	// Style index constants (used by MdRenderer and ChatWindow).
	enum {
		kStyleDefault    = 0,
		kStyleUserLabel  = 1,
		kStyleModelLabel = 2,
		kStyleToolLine   = 3,
		kStyleError      = 4,
		kStyleCode       = 5,   // inline code / monospace
		kStyleH1         = 6,
		kStyleH2         = 7,
		kStyleH3         = 8,
		kStyleLink       = 9,
		kStyleBlockquote = 10,
		kStyleDim        = 11,
		kStyleCodeLang   = 12,  // language tag above a code block
	};

	SciOutput(const char* name,
	          styling::Theme* theme,
	          styling::LanguageSet* langSet);

	// ── Text output ──────────────────────────────────────────────────────────

	// Append text with a fixed style index (see kStyle* above).
	void	AppendText(const std::string& text, int style = kStyleDefault);

	// Append a fenced code block. Applies Lexilla syntax highlighting
	// for `lang` if the language is known; falls back to kStyleCode.
	void	AppendCodeBlock(const std::string& code, const std::string& lang);

	// Erase all content.
	void	Clear();

	// Scroll to the very end of the document.
	void	ScrollToEnd();

	// True when the scroll position is within `threshold` lines of the end.
	bool	IsNearBottom(int threshold = 3) const;

	// ── BScintillaView overrides ─────────────────────────────────────────────
	void	AttachedToWindow() override;

private:
	void	_ConfigureBaseStyles();
	long	_Sci(unsigned int msg, unsigned long w = 0, long l = 0);

	styling::Theme*       fTheme   = nullptr;
	styling::LanguageSet* fLangSet = nullptr;
	bool fConfigured = false;
};
