// sci_output.cpp — BScintillaView-based chat output widget.
//
// One instance lives for the lifetime of the ChatWindow and receives
// all streamed text (prose, tool lines, code blocks). Styles are applied
// as text is appended via SCI_STARTSTYLING / SCI_SETSTYLING.

#include "sci_output.h"

#include <algorithm>
#include <cstring>

#include "code_styler.h"

// ---------------------------------------------------------------------------
// SCI_ message numbers (no Scintilla headers available as a package).
// ---------------------------------------------------------------------------
namespace {

// Text.
constexpr unsigned int kAddText            = 2001;
constexpr unsigned int kClearAll           = 2004;
constexpr unsigned int kGetLength          = 2006;
constexpr unsigned int kSetReadOnly        = 2171;
constexpr unsigned int kGotoPos            = 2025;
constexpr unsigned int kGetLineCount       = 2154;
constexpr unsigned int kGetFirstVisibleLine = 2152;
constexpr unsigned int kLinesOnScreen      = 2193;
constexpr unsigned int kScrollToEnd        = 2629; // SCI_SCROLLTOEND (3.x+)

// Styling.
constexpr unsigned int kStartStyling       = 2032;
constexpr unsigned int kSetStyling         = 2033;
constexpr unsigned int kStyleSetFore       = 2051;
constexpr unsigned int kStyleSetBack       = 2052;
constexpr unsigned int kStyleSetBold       = 2053;
constexpr unsigned int kStyleSetItalic     = 2056;
constexpr unsigned int kStyleSetSize       = 2055;
constexpr unsigned int kStyleClearAll      = 2050;
constexpr unsigned int kStyleResetDefault  = 2058;

// Lexer.
constexpr unsigned int kSetLexerLanguage   = 4006;
constexpr unsigned int kSetKeyWords        = 2005;
constexpr unsigned int kColourise          = 4003;

// View.
constexpr unsigned int kSetMarginWidthN    = 2242;
constexpr unsigned int kSetWrapMode        = 2268;
constexpr unsigned int kSetScrollWidthTracking = 2516;
constexpr unsigned int kSetCaretFore       = 2069;
constexpr unsigned int kSetSelBack         = 2068;
constexpr unsigned int kSetSelFore         = 2067;
constexpr unsigned int kSetCaretLineBack   = 2098;
constexpr unsigned int kSetCaretLineVisible = 2096;
constexpr unsigned int kSetEndAtLastLine   = 2277;

// Colours.
constexpr long kChatBg     = 0x1C1C1E; // BGR in Scintilla = 0xRRGGBB LE
constexpr long kTextColor  = 0xDCDCDC;

// Helper: pack RGB → Scintilla COLORREF (BGR order in the int).
inline long rgb(int r, int g, int b)
{
	return static_cast<long>((b << 16) | (g << 8) | r);
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

SciOutput::SciOutput(const char* name,
                     styling::Theme* theme,
                     styling::LanguageSet* langSet)
	: BScintillaView(name, B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS,
	                 false, true, B_NO_BORDER)
	, fTheme(theme)
	, fLangSet(langSet)
{
}

// ---------------------------------------------------------------------------
// Configure — call explicitly after _BuildLayout() to apply styles.
// ---------------------------------------------------------------------------

void SciOutput::Configure()
{
	if (fConfigured) return;
	fConfigured = true;
	_ConfigureBaseStyles();
}

// ---------------------------------------------------------------------------
// _ConfigureBaseStyles
// ---------------------------------------------------------------------------

void SciOutput::_ConfigureBaseStyles()
{
	// Read-only, no line numbers, word-wrap.
	_Sci(kSetReadOnly, 1);
	_Sci(kSetMarginWidthN, 0, 0);  // line number margin = 0
	_Sci(kSetMarginWidthN, 1, 0);  // symbol margin = 0
	_Sci(kSetWrapMode, 2);         // SC_WRAP_WORD = 2
	_Sci(kSetScrollWidthTracking, 1);
	_Sci(kSetEndAtLastLine, 1);

	// Default style — dark background, light text.
	_Sci(kStyleResetDefault);
	_Sci(kStyleSetBack, 32, rgb(0x1C, 0x1C, 0x1E));
	_Sci(kStyleSetFore, 32, rgb(0xDC, 0xDC, 0xDC));
	_Sci(kStyleSetSize, 32, 10);
	_Sci(kStyleClearAll); // propagate STYLE_DEFAULT to all styles

	// Caret and selection.
	_Sci(kSetCaretFore, rgb(0xFF, 0xFF, 0xFF));
	_Sci(kSetCaretLineVisible, 0);  // no caret line highlight in read-only mode
	_Sci(kSetSelBack, 1, rgb(0x44, 0x44, 0x66));

	// ── Named styles ─────────────────────────────────────────────────────────

	// Style 1: user label — sky blue.
	_Sci(kStyleSetFore, kStyleUserLabel,  rgb(0x56, 0xB4, 0xE9));
	_Sci(kStyleSetBold, kStyleUserLabel,  1);

	// Style 2: model label — warm orange.
	_Sci(kStyleSetFore, kStyleModelLabel, rgb(0xCC, 0x79, 0x5A));
	_Sci(kStyleSetBold, kStyleModelLabel, 1);

	// Style 3: tool/dim lines — grey.
	_Sci(kStyleSetFore, kStyleToolLine,   rgb(0x82, 0x82, 0x8C));

	// Style 4: error — red.
	_Sci(kStyleSetFore, kStyleError,      rgb(0xE6, 0x4B, 0x4B));

	// Style 5: inline code — warm monospace (uses default font).
	_Sci(kStyleSetFore, kStyleCode, rgb(0xC8, 0xC8, 0xA0));

	// Style 6: H1 — golden, larger.
	_Sci(kStyleSetFore, kStyleH1,         rgb(0xFF, 0xC8, 0x64));
	_Sci(kStyleSetBold, kStyleH1,         1);
	_Sci(kStyleSetSize, kStyleH1,         16);

	// Style 7: H2 — sky blue, medium.
	_Sci(kStyleSetFore, kStyleH2,         rgb(0xB4, 0xDC, 0xFF));
	_Sci(kStyleSetBold, kStyleH2,         1);
	_Sci(kStyleSetSize, kStyleH2,         13);

	// Style 8: H3 — soft green.
	_Sci(kStyleSetFore, kStyleH3,         rgb(0xC8, 0xFF, 0xC8));
	_Sci(kStyleSetBold, kStyleH3,         1);
	_Sci(kStyleSetSize, kStyleH3,         12);

	// Style 9: link — blue underline.
	_Sci(kStyleSetFore, kStyleLink,       rgb(0x56, 0xB4, 0xE9));

	// Style 10: blockquote — muted italic.
	_Sci(kStyleSetFore,   kStyleBlockquote, rgb(0x96, 0x96, 0x96));
	_Sci(kStyleSetItalic, kStyleBlockquote, 1);

	// Style 11: dim (bullets, list numbers).
	_Sci(kStyleSetFore, kStyleDim,        rgb(0x78, 0x78, 0x78));

	// Style 12: code language tag — italic muted purple.
	_Sci(kStyleSetFore,   kStyleCodeLang, rgb(0x78, 0x78, 0x8C));
	_Sci(kStyleSetItalic, kStyleCodeLang, 1);

	// No lexer for the base document (plain text).
	_Sci(kSetLexerLanguage, 0, reinterpret_cast<long>("null"));
}

// ---------------------------------------------------------------------------
// AppendText
// ---------------------------------------------------------------------------

void SciOutput::AppendText(const std::string& text, int style)
{
	if (text.empty() || !fConfigured) return;

	// Temporarily make writable.
	_Sci(kSetReadOnly, 0);

	const int startPos = static_cast<int>(_Sci(kGetLength));

	// Append the bytes.
	_Sci(kAddText, static_cast<unsigned long>(text.size()),
	     reinterpret_cast<long>(text.c_str()));

	// Apply the style to the appended bytes.
	_Sci(kStartStyling, startPos);
	_Sci(kSetStyling, static_cast<unsigned long>(text.size()), style);

	_Sci(kSetReadOnly, 1);
}

// ---------------------------------------------------------------------------
// AppendCodeBlock
// ---------------------------------------------------------------------------

void SciOutput::AppendCodeBlock(const std::string& code, const std::string& lang)
{
	if (code.empty() || !fConfigured) return;

	// Language tag line.
	if (!lang.empty())
		AppendText(lang + "\n", kStyleCodeLang);

	// Code body — plain kStyleCode for now.
	// Syntax highlighting (Stage 2) will run Lexilla externally and apply
	// style bytes into Scintilla's style buffer at the correct offset.
	std::string body = code;
	if (body.empty() || body.back() != '\n') body += '\n';
	AppendText(body, kStyleCode);

	// Blank line after block.
	AppendText("\n", kStyleDefault);
}

// ---------------------------------------------------------------------------
// Clear / ScrollToEnd / IsNearBottom
// ---------------------------------------------------------------------------

void SciOutput::Clear()
{
	if (!fConfigured) return;
	_Sci(kSetReadOnly, 0);
	_Sci(kClearAll);
	_Sci(kSetReadOnly, 1);
}

void SciOutput::ScrollToEnd()
{
	if (!fConfigured) return;
	// SCI_SCROLLTOEND (2629) moves view to last line.
	_Sci(kScrollToEnd);
}

bool SciOutput::IsNearBottom(int threshold) const
{
	const long firstVisible = const_cast<SciOutput*>(this)->_Sci(kGetFirstVisibleLine);
	const long lineCount    = const_cast<SciOutput*>(this)->_Sci(kGetLineCount);
	const long linesOnScr   = const_cast<SciOutput*>(this)->_Sci(kLinesOnScreen);
	return (lineCount - firstVisible - linesOnScr) <= threshold;
}

// ---------------------------------------------------------------------------
// _Sci
// ---------------------------------------------------------------------------

long SciOutput::_Sci(unsigned int msg, unsigned long w, long l)
{
	return SendMessage(msg, w, l);
}
