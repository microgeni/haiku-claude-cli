#ifndef HAIKU_CLAUDE_CLI_MD_RENDERER_H
#define HAIKU_CLAUDE_CLI_MD_RENDERER_H

#include <functional>
#include <string>
#include <vector>

#include <TextView.h>

#include "md_text.h"  // md::StripHtml / table-parsing (pure logic).

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

	// Flush any buffered partial line and any pending table (call at end of turn).
	void Flush();

	// URLs harvested from [text](url) markdown links.
	const std::vector<std::string>& Urls() const { return fUrls; }
	void ClearUrls() { fUrls.clear(); }

	// Public so ChatWindow can append styled runs directly (e.g. code blocks).
	void AppendRun(const Run& r);
	void ScrollToEnd();

	// Enable/disable the renderer's internal scroll-to-end calls. When the
	// user has scrolled up to read earlier text, ChatWindow disables this so
	// streaming replies don't yank the view back to the bottom (and fight
	// the user's scroll position). Defaults to true.
	void SetAutoScroll(bool on) { fAutoScroll = on; }

	// Set the zoom multiplier applied to every run's font size at render
	// time. New text (streamed or appended) is emitted pre-scaled to this
	// factor so it matches the user's chosen zoom immediately, rather than
	// being retro-scaled after the turn. Defaults to 1.0.
	void SetZoom(float zoom) { fZoom = (zoom > 0.0f) ? zoom : 1.0f; }
	float Zoom() const { return fZoom; }

private:
	// Render one complete line.
	void RenderLine(const std::string& line);

	// Render inline spans within `text` (bold/italic/code/links).
	// `baseRun` carries the heading/blockquote context.
	void RenderInline(const std::string& text, const Run& base);

	// Append a simple horizontal rule.
	void AppendHRule();

	// ── Table support ───────────────────────────────────────────────────────
	// Tables are buffered line-by-line until a non-table line arrives,
	// then flushed as a single box-drawing block so column widths can be
	// computed from the full table before any output is emitted.

	// Returns true if `line` looks like a markdown table row (contains '|').
	static bool IsTableRow(const std::string& line);

	// Returns true if `line` is a table separator row (|---|---|).
	static bool IsSeparatorRow(const std::string& line);

	// Split a pipe-delimited table row into trimmed cell strings.
	static std::vector<std::string> SplitCells(const std::string& line);

	// Emit the buffered table to the BTextView using box-drawing characters.
	void FlushTable();

	BTextView*           fView;
	std::string          fLineBuf;   // partial line accumulator
	std::vector<std::string> fUrls;

	// Zoom multiplier applied to every run's font size (see SetZoom).
	float fZoom = 1.0f;

	// When false, ScrollToEnd() is a no-op so the renderer never yanks the
	// view to the bottom (see SetAutoScroll).
	bool fAutoScroll = true;

	// Track blank-line state for paragraph spacing.
	bool fLastWasBlank = false;

	// Table accumulator state.
	bool fInTable = false;
	// Parsed rows (each is a vector of trimmed cell strings).
	// Separator rows are recorded as position markers, not stored as cells.
	std::vector<std::vector<std::string>> fTableRows;
	// Index into fTableRows where the header/body split falls.
	// -1 = no separator seen yet (row 0 is treated as header).
	int fSeparatorRow = -1;
};

// Strip HTML tags and decode common entities from a string. The declaration
// and definition now live in md_text.h / md_text.cpp (pure, unit-tested);
// md_text.h is included at the top of this header so existing callers of
// md::StripHtml via "md_renderer.h" keep compiling unchanged.

} // namespace md

#endif // HAIKU_CLAUDE_CLI_MD_RENDERER_H
