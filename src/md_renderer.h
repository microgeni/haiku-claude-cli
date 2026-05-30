#ifndef HAIKU_CLAUDE_CLI_MD_RENDERER_H
#define HAIKU_CLAUDE_CLI_MD_RENDERER_H

#include <functional>
#include <string>
#include <vector>

#include <TextView.h>

// MdRenderer — inline markdown renderer for BTextView.
//
// Parses a stream of text chunks (as they arrive from the model) and
// emits styled runs to a BTextView. Handles:
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
// Fenced code blocks (triple backtick) are handled by ChatWindow's
// _ProcessChunk / _FlushCodeBlock; MdRenderer never sees them.
//
// The renderer is line-oriented: complete lines are rendered when a
// '\n' arrives. Partial lines are buffered in fLineBuf.

namespace md {

// Style descriptor for a single text run sent to BTextView.
struct Run {
	std::string text;
	// Font modifiers (combined via BFont::SetFace).
	bool bold      = false;
	bool italic    = false;
	bool monospace = false;  // use be_fixed_font
	float sizeScale = 1.0f; // multiplier on be_plain_font pointSize
	// Colour (0 = inherit from view default).
	rgb_color color = { 0, 0, 0, 0 };
	bool hasColor   = false;
	// Underline (for links).
	bool underline  = false;
};

class MdRenderer {
public:
	explicit MdRenderer(BTextView* view);

	// Feed a chunk of text (may contain newlines; may be partial line).
	// Renders complete lines immediately; buffers the last partial line.
	void Write(const std::string& chunk);

	// Flush any buffered partial line (call at end of turn).
	void Flush();

	// URLs harvested from [text](url) markdown links.
	const std::vector<std::string>& Urls() const { return fUrls; }
	void ClearUrls() { fUrls.clear(); }

private:
	// Render one complete line.
	void RenderLine(const std::string& line);

	// Append a Run to fView, advancing the insert position.
	void AppendRun(const Run& r);

	// Render inline spans within `text` (bold/italic/code/links).
	// `baseRun` carries the heading/blockquote context.
	void RenderInline(const std::string& text, const Run& base);

	// Append a simple horizontal rule.
	void AppendHRule();

	// Scroll fView to its end.
	void ScrollToEnd();

	BTextView*           fView;
	std::string          fLineBuf;   // partial line accumulator
	std::vector<std::string> fUrls;

	// Track blank-line state for paragraph spacing.
	bool fLastWasBlank = false;
};

// Strip HTML tags and decode common entities from a string.
// Used to make WebFetch output readable in plain BTextView.
std::string StripHtml(const std::string& html);

} // namespace md

#endif // HAIKU_CLAUDE_CLI_MD_RENDERER_H
