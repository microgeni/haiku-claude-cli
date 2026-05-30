#ifndef HAIKU_CLAUDE_CLI_MD_RENDERER_H
#define HAIKU_CLAUDE_CLI_MD_RENDERER_H

#include <functional>
#include <string>
#include <vector>

#include <TextView.h>

// Forward declaration — avoids pulling BScintillaView into every consumer.
class SciOutput;

// MdRenderer — inline markdown renderer for BTextView or SciOutput.
//
// Parses a stream of text chunks (as they arrive from the model) and
// emits styled runs to the output surface. Handles:
//
//   # / ## / ### headings   — larger bold font
//   **bold** / __bold__     — bold
//   *italic* / _italic_     — italic
//   `inline code`           — monospace, dim background
//   - / * / + bullet lists  — • prefix, indented
//   1. numbered lists       — number preserved, indented
//   > blockquote            — dim, indented
//   [text](url)             — underlined; url collected in fUrls
//   --- / *** / ___ hrule   — ────────────── separator line
//   Paragraphs              — blank line → extra newline
//
// Fenced code blocks are handled by ChatWindow::_FlushCodeBlock;
// MdRenderer never sees them directly.
//
// Line-oriented: complete lines rendered on '\n'; partial lines buffered.

namespace md {

// Style descriptor for a single text run.
struct Run {
	std::string text;
	// BTextView path font modifiers.
	bool      bold       = false;
	bool      italic     = false;
	bool      monospace  = false;
	float     sizeScale  = 1.0f;
	rgb_color color      = { 0, 0, 0, 0 };
	bool      hasColor   = false;
	bool      underline  = false;
	// SciOutput style override (SciOutput::kStyle* constants).
	// When hasSciStyle is true this overrides auto-derivation.
	int  sciStyle    = 0;
	bool hasSciStyle = false;
};

class MdRenderer {
public:
	// BTextView backend — existing behaviour.
	explicit MdRenderer(BTextView* view);

	// SciOutput backend — routes all output through SciOutput::AppendText.
	explicit MdRenderer(SciOutput* output);

	// Feed a text chunk (may span multiple lines).
	void Write(const std::string& chunk);

	// Flush any buffered partial line.
	void Flush();

	const std::vector<std::string>& Urls() const { return fUrls; }
	void ClearUrls() { fUrls.clear(); }

	// Public so ChatWindow can append styled runs directly (e.g. code blocks).
	void AppendRun(const Run& r);
	void ScrollToEnd();

private:
	void RenderLine(const std::string& line);
	void RenderInline(const std::string& text, const Run& base);
	void AppendHRule();

	BTextView* fView      = nullptr;   // BTextView backend (null when SciOutput)
	SciOutput* fSciOutput = nullptr;   // SciOutput backend (null when BTextView)

	std::string              fLineBuf;
	std::vector<std::string> fUrls;
	bool                     fLastWasBlank = false;
};

// Strip HTML tags from a string (for WebFetch output).
std::string StripHtml(const std::string& html);

} // namespace md

#endif // HAIKU_CLAUDE_CLI_MD_RENDERER_H
