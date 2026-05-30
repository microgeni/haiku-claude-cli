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
	// View behaviour — these are all integer params, no string pointers.
	_Sci(kSetReadOnly, 1);
	_Sci(kSetMarginWidthN, 0, 0);
	_Sci(kSetMarginWidthN, 1, 0);
	_Sci(kSetWrapMode, 2);
	_Sci(kSetScrollWidthTracking, 1);
	_Sci(kSetEndAtLastLine, 1);
	_Sci(kSetCaretLineVisible, 0);
	_Sci(kSetCaretFore, rgb(0xFF, 0xFF, 0xFF));
	_Sci(kSetSelBack, 1, rgb(0x44, 0x44, 0x66));

	// Background and foreground for all styles 0-40.
	// Set individually — avoids SCI_STYLECLEARALL which triggers
	// internal font propagation and crashes on this Scintilla build.
	const long bg = rgb(0x1C, 0x1C, 0x1E);
	const long fg = rgb(0xDC, 0xDC, 0xDC);
	for (int s = 0; s <= 40; ++s) {
		_Sci(kStyleSetBack, static_cast<unsigned long>(s), bg);
		_Sci(kStyleSetFore, static_cast<unsigned long>(s), fg);
	}

	// Named prose styles — fore colour only (back inherited above).
	_Sci(kStyleSetFore, kStyleUserLabel,    rgb(0x56, 0xB4, 0xE9));
	_Sci(kStyleSetBold, kStyleUserLabel,    1);
	_Sci(kStyleSetFore, kStyleModelLabel,   rgb(0xCC, 0x79, 0x5A));
	_Sci(kStyleSetBold, kStyleModelLabel,   1);
	_Sci(kStyleSetFore, kStyleToolLine,     rgb(0x82, 0x82, 0x8C));
	_Sci(kStyleSetFore, kStyleError,        rgb(0xE6, 0x4B, 0x4B));
	_Sci(kStyleSetFore, kStyleCode,         rgb(0xC8, 0xC8, 0xA0));
	_Sci(kStyleSetFore, kStyleH1,           rgb(0xFF, 0xC8, 0x64));
	_Sci(kStyleSetBold, kStyleH1,           1);
	_Sci(kStyleSetFore, kStyleH2,           rgb(0xB4, 0xDC, 0xFF));
	_Sci(kStyleSetBold, kStyleH2,           1);
	_Sci(kStyleSetFore, kStyleH3,           rgb(0xC8, 0xFF, 0xC8));
	_Sci(kStyleSetBold, kStyleH3,           1);
	_Sci(kStyleSetFore, kStyleLink,         rgb(0x56, 0xB4, 0xE9));
	_Sci(kStyleSetFore,   kStyleBlockquote, rgb(0x96, 0x96, 0x96));
	_Sci(kStyleSetItalic, kStyleBlockquote, 1);
	_Sci(kStyleSetFore, kStyleDim,          rgb(0x78, 0x78, 0x78));
	_Sci(kStyleSetFore,   kStyleCodeLang,   rgb(0x78, 0x78, 0x8C));
	_Sci(kStyleSetItalic, kStyleCodeLang,   1);
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
