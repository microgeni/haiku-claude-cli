#include "md_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace md {

// ---------------------------------------------------------------------------
// Colour constants (dark theme palette matching gui styling)
// ---------------------------------------------------------------------------
namespace {

const rgb_color kColorText       = { 220, 220, 220, 255 };
const rgb_color kColorDim        = { 140, 140, 140, 255 };
const rgb_color kColorCode       = { 200, 200, 160, 255 }; // warm mono
const rgb_color kColorCodeBg     = {  50,  50,  45, 255 }; // subtle bg
const rgb_color kColorH1         = { 255, 200, 100, 255 }; // golden
const rgb_color kColorH2         = { 180, 220, 255, 255 }; // sky blue
const rgb_color kColorH3         = { 200, 255, 200, 255 }; // soft green
const rgb_color kColorBlockquote = { 150, 150, 150, 255 };
const rgb_color kColorLink       = {  86, 180, 233, 255 }; // same as user label
const rgb_color kColorHRule      = { 100, 100, 100, 255 };

// Table colours.
const rgb_color kColorTableBorder  = {  80, 130, 130, 255 }; // muted teal
const rgb_color kColorTableHeader  = { 255, 255, 255, 255 }; // white bold
const rgb_color kColorTableCell    = { 215, 215, 220, 255 }; // near-white
const rgb_color kColorTableAlt     = { 175, 175, 185, 255 }; // slightly dimmer

} // namespace

// ---------------------------------------------------------------------------
// MdRenderer
// ---------------------------------------------------------------------------

MdRenderer::MdRenderer(BTextView* view)
	: fView(view)
{
}

void MdRenderer::Write(const std::string& chunk)
{
	fLineBuf += chunk;
	size_t pos = 0;
	while (true) {
		const size_t nl = fLineBuf.find('\n', pos);
		if (nl == std::string::npos) {
			// No complete line yet; leave remainder in buffer.
			fLineBuf = fLineBuf.substr(pos);
			return;
		}
		const std::string line = fLineBuf.substr(pos, nl - pos);
		pos = nl + 1;
		RenderLine(line);
	}
}

void MdRenderer::Flush()
{
	// Flush any pending table before the partial line.
	if (fInTable)
		FlushTable();

	if (!fLineBuf.empty()) {
		RenderLine(fLineBuf);
		fLineBuf.clear();
	}
}

// ---------------------------------------------------------------------------
// Table helpers
// ---------------------------------------------------------------------------

// A line is a table row if it contains at least one '|' and, when trimmed,
// starts with '|' or has ' | ' inside it.
bool MdRenderer::IsTableRow(const std::string& line)
{
	if (line.find('|') == std::string::npos) return false;
	// Trim leading whitespace.
	size_t start = 0;
	while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
		++start;
	if (start < line.size() && line[start] == '|') return true;
	// Has an embedded " | ".
	return line.find(" | ") != std::string::npos;
}

// A separator row contains only '|', '-', ':', and whitespace, with at
// least one '-'.
bool MdRenderer::IsSeparatorRow(const std::string& line)
{
	bool hasDash = false;
	for (char c : line) {
		if (c == '-') { hasDash = true; continue; }
		if (c == '|' || c == ':' || c == ' ' || c == '\t') continue;
		return false;
	}
	return hasDash;
}

// Split a pipe-delimited row into trimmed cell strings, stripping leading
// and trailing '|' characters first.
std::vector<std::string> MdRenderer::SplitCells(const std::string& line)
{
	// Trim leading/trailing whitespace then strip outer '|' chars.
	size_t a = 0;
	while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
	size_t b = line.size();
	while (b > a && (line[b-1] == ' ' || line[b-1] == '\t')) --b;
	std::string t = line.substr(a, b - a);
	if (!t.empty() && t.front() == '|') t = t.substr(1);
	if (!t.empty() && t.back()  == '|') t.pop_back();

	std::vector<std::string> cells;
	std::istringstream ss(t);
	std::string cell;
	while (std::getline(ss, cell, '|')) {
		// Trim each cell.
		size_t ca = cell.find_first_not_of(" \t");
		size_t cb = cell.find_last_not_of(" \t");
		cells.push_back((ca == std::string::npos) ? "" : cell.substr(ca, cb - ca + 1));
	}
	return cells;
}

// Flush the accumulated table rows as a box-drawing table into the BTextView.
void MdRenderer::FlushTable()
{
	if (fTableRows.empty()) {
		fInTable = false;
		fSeparatorRow = -1;
		return;
	}

	// ── 1. Normalise column count ─────────────────────────────────────────
	size_t nCols = 0;
	for (const auto& row : fTableRows)
		nCols = std::max(nCols, row.size());
	if (nCols == 0) {
		fInTable = false;
		fSeparatorRow = -1;
		fTableRows.clear();
		return;
	}
	for (auto& row : fTableRows)
		while (row.size() < nCols) row.push_back("");

	// ── 2. Compute column widths (bytes; works for ASCII content) ─────────
	std::vector<size_t> colW(nCols, 1);
	for (const auto& row : fTableRows)
		for (size_t c = 0; c < nCols; ++c)
			colW[c] = std::max(colW[c], row[c].size());

	// ── 3. Box-drawing helpers ────────────────────────────────────────────
	// Build a horizontal border line.
	// left/mid/right are UTF-8 encoded single box-drawing characters.
	// fill is the horizontal bar character (─ or ═).
	auto makeBorder = [&](const char* left, const char* mid,
	                      const char* right, const char* fill) -> std::string {
		std::string s = left;
		for (size_t c = 0; c < nCols; ++c) {
			for (size_t k = 0; k < colW[c] + 2; ++k) s += fill;
			s += (c + 1 < nCols) ? mid : right;
		}
		return s + "\n";
	};

	// Pad/truncate a cell to exactly `w` visible characters.
	auto padCell = [](const std::string& s, size_t w) -> std::string {
		if (s.size() >= w) return s.substr(0, w);
		return s + std::string(w - s.size(), ' ');
	};

	// ── 4. Determine header boundary ─────────────────────────────────────
	// Rows before fSeparatorRow are header rows; if no separator was seen
	// treat the first row as the header.
	size_t headerEnd = (fSeparatorRow > 0)
	    ? static_cast<size_t>(fSeparatorRow)
	    : 1;

	// ── 5. Emit fonts ─────────────────────────────────────────────────────
	// Use monospace for the whole table so columns align.
	BFont monoPlain(be_fixed_font);
	monoPlain.SetSize(be_fixed_font->Size());

	BFont monoBold(be_fixed_font);
	monoBold.SetSize(be_fixed_font->Size());
	monoBold.SetFace(B_BOLD_FACE);

	// Helper: append a string with the given font and colour.
	auto emit = [&](const std::string& text, const BFont& font, rgb_color color) {
		if (text.empty()) return;
		text_run_array* tra = static_cast<text_run_array*>(
			malloc(sizeof(text_run_array) + sizeof(text_run)));
		if (!tra) {
			fView->Insert(text.c_str(), static_cast<int32>(text.size()));
			return;
		}
		tra->count          = 1;
		tra->runs[0].offset = 0;
		tra->runs[0].font   = font;
		tra->runs[0].color  = color;
		const int32 pos = fView->TextLength();
		fView->Insert(pos, text.c_str(), static_cast<int32>(text.size()), tra);
		free(tra);
	};

	// ── 6. Draw the table ─────────────────────────────────────────────────
	// Opening newline for separation from previous content.
	emit("\n", monoPlain, kColorTableCell);

	// Top border:  ┌───────┬───────┐
	//              U+250C  U+252C  U+2510  U+2500
	emit(makeBorder("┌", "┬", "┐", "─"), monoPlain, kColorTableBorder);

	for (size_t r = 0; r < fTableRows.size(); ++r) {
		const auto& row    = fTableRows[r];
		const bool  isHdr  = (r < headerEnd);

		// Row cells:  │ cell  │ cell  │
		// Emit '│' in border colour, cell text in cell colour.
		for (size_t c = 0; c < nCols; ++c) {
			emit("│ ", monoPlain, kColorTableBorder);
			const std::string cellText = padCell(row[c], colW[c]);
			if (isHdr)
				emit(cellText, monoBold, kColorTableHeader);
			else
				emit(cellText, monoPlain,
				     (r % 2 == 0) ? kColorTableCell : kColorTableAlt);
			emit(" ", monoPlain, kColorTableBorder);
		}
		emit("│\n", monoPlain, kColorTableBorder);

		// After the last header row: double horizontal separator  ╞═══╪═══╡
		//                                               U+255E  U+256A  U+2561  U+2550
		if (isHdr && r + 1 == headerEnd)
			emit(makeBorder("╞", "╪", "╡", "═"), monoPlain, kColorTableBorder);
		// Between body rows: light separator  ├───┼───┤
		//                             U+251C  U+253C  U+2524
		else if (!isHdr && r + 1 < fTableRows.size())
			emit(makeBorder("├", "┼", "┤", "─"), monoPlain, kColorTableBorder);
	}

	// Bottom border:  └───────┴───────┘
	//                 U+2514  U+2534  U+2518
	emit(makeBorder("└", "┴", "┘", "─"), monoPlain, kColorTableBorder);
	emit("\n", monoPlain, kColorTableCell);

	// Scroll so the bottom of the table is visible.
	fView->ScrollToOffset(fView->TextLength());

	// ── 7. Reset state ────────────────────────────────────────────────────
	fInTable = false;
	fSeparatorRow = -1;
	fTableRows.clear();
}

// ---------------------------------------------------------------------------
// AppendRun — emit a styled text run to BTextView
// ---------------------------------------------------------------------------

void MdRenderer::AppendRun(const Run& r)
{
	if (r.text.empty()) return;

	// Build font.
	BFont font(r.monospace ? be_fixed_font : be_plain_font);
	if (r.sizeScale != 1.0f) {
		font.SetSize(font.Size() * r.sizeScale);
	}
	uint16 face = B_REGULAR_FACE;
	if (r.bold)   face |= B_BOLD_FACE;
	if (r.italic) face |= B_ITALIC_FACE;
	if (r.underline) face |= B_UNDERSCORE_FACE;
	if (face != B_REGULAR_FACE) font.SetFace(face);

	// Build colour.
	rgb_color color = kColorText;
	if (r.hasColor) color = r.color;
	else if (r.monospace) color = kColorCode;

	text_run_array* tra = static_cast<text_run_array*>(
		malloc(sizeof(text_run_array) + sizeof(text_run)));
	if (!tra) {
		fView->Insert(r.text.c_str(), static_cast<int32>(r.text.size()));
		return;
	}
	tra->count       = 1;
	tra->runs[0].offset = 0;
	tra->runs[0].font   = font;
	tra->runs[0].color  = color;

	const int32 start = fView->TextLength();
	fView->Insert(start, r.text.c_str(), static_cast<int32>(r.text.size()), tra);
	free(tra);

	// Optional code background via view colour behind the text.
	// BTextView doesn't support per-run background. We skip it for now;
	// the warm foreground colour is enough to distinguish inline code.
}

void MdRenderer::ScrollToEnd()
{
	fView->ScrollToOffset(fView->TextLength());
}

// ---------------------------------------------------------------------------
// AppendHRule
// ---------------------------------------------------------------------------

void MdRenderer::AppendHRule()
{
	// A row of U+2500 BOX DRAWINGS LIGHT HORIZONTAL characters.
	// Width ≈ view width in chars (approximate at 8px/char).
	const float viewW = fView->Bounds().Width();
	const int   nchars = std::max(10, static_cast<int>(viewW / 8.0f));
	std::string rule;
	rule.reserve(static_cast<size_t>(nchars) * 3 + 1);
	for (int i = 0; i < nchars; ++i) rule += "\xE2\x94\x80"; // U+2500 ─
	rule += '\n';

	Run r;
	r.text     = rule;
	r.hasColor = true;
	r.color    = kColorHRule;
	AppendRun(r);
}

// ---------------------------------------------------------------------------
// RenderInline — handle **bold**, *italic*, `code`, [link](url) within a line
// ---------------------------------------------------------------------------

void MdRenderer::RenderInline(const std::string& text, const Run& base)
{
	// Simple state-machine parser. We scan left to right; when we find
	// a marker we emit the text before it and toggle the corresponding
	// state. Nested markers are not supported (matches most Markdown).

	size_t i = 0;
	const size_t n = text.size();

	// Accumulated plain-text span since the last marker.
	std::string plain;

	auto flush_plain = [&]() {
		if (plain.empty()) return;
		Run r = base;
		r.text = plain;
		AppendRun(r);
		plain.clear();
	};

	while (i < n) {
		// ── Inline code: `...` ─────────────────────────────────────────
		if (text[i] == '`') {
			const size_t end = text.find('`', i + 1);
			if (end != std::string::npos) {
				flush_plain();
				Run r = base;
				r.text      = text.substr(i + 1, end - i - 1);
				r.monospace = true;
				r.hasColor  = true;
				r.color     = kColorCode;
				AppendRun(r);
				i = end + 1;
				continue;
			}
		}

		// ── Bold: **...** or __...__ ───────────────────────────────────
		if (i + 1 < n &&
		    ((text[i] == '*' && text[i+1] == '*') ||
		     (text[i] == '_' && text[i+1] == '_'))) {
			const char delim = text[i];
			const size_t end = text.find(std::string(2, delim), i + 2);
			if (end != std::string::npos) {
				flush_plain();
				Run r = base;
				r.text = text.substr(i + 2, end - i - 2);
				r.bold = true;
				AppendRun(r);
				i = end + 2;
				continue;
			}
		}

		// ── Italic: *...* or _..._ (single) ───────────────────────────
		if ((text[i] == '*' || text[i] == '_') &&
		    (i == 0 || text[i-1] != text[i])) {
			const char delim = text[i];
			const size_t end = text.find(delim, i + 1);
			if (end != std::string::npos &&
			    (end + 1 >= n || text[end + 1] != delim)) {
				flush_plain();
				Run r = base;
				r.text   = text.substr(i + 1, end - i - 1);
				r.italic = true;
				AppendRun(r);
				i = end + 1;
				continue;
			}
		}

		// ── Link: [text](url) ──────────────────────────────────────────
		if (text[i] == '[') {
			const size_t close = text.find(']', i + 1);
			if (close != std::string::npos && close + 1 < n &&
			    text[close + 1] == '(') {
				const size_t urlEnd = text.find(')', close + 2);
				if (urlEnd != std::string::npos) {
					flush_plain();
					const std::string linkText = text.substr(i + 1, close - i - 1);
					const std::string url      = text.substr(close + 2, urlEnd - close - 2);
					fUrls.push_back(url);

					Run r = base;
					r.text      = linkText;
					r.hasColor  = true;
					r.color     = kColorLink;
					r.underline = true;
					AppendRun(r);
					i = urlEnd + 1;
					continue;
				}
			}
		}

		plain += text[i++];
	}
	flush_plain();
}

// ---------------------------------------------------------------------------
// RenderLine — determine line type, then call RenderInline for inline spans
// ---------------------------------------------------------------------------

void MdRenderer::RenderLine(const std::string& rawLine)
{
	// Strip trailing CR if present.
	std::string line = rawLine;
	if (!line.empty() && line.back() == '\r') line.pop_back();

	// ── Table accumulation ─────────────────────────────────────────────────
	// Buffer table rows until a non-table line arrives, then flush the
	// complete table so column widths can be computed before any output.
	if (IsTableRow(line)) {
		fInTable = true;
		fLastWasBlank = false;
		if (IsSeparatorRow(line)) {
			// Record where the header/body split is, but don't store cells.
			if (fSeparatorRow < 0)
				fSeparatorRow = static_cast<int>(fTableRows.size());
		} else {
			fTableRows.push_back(SplitCells(line));
		}
		return; // accumulate; don't render yet
	}

	// Non-table line: flush any accumulated table first.
	if (fInTable)
		FlushTable();

	// ── Blank line: paragraph break ────────────────────────────────────────
	if (line.empty()) {
		if (!fLastWasBlank) {
			Run r;
			r.text = "\n";
			AppendRun(r);
		}
		fLastWasBlank = true;
		ScrollToEnd();
		return;
	}
	fLastWasBlank = false;

	// ── Horizontal rule: ---, ***, ___ ────────────────────────────────────
	if (line == "---" || line == "***" || line == "___" ||
	    line == "- - -" || line == "* * *") {
		AppendHRule();
		ScrollToEnd();
		return;
	}

	// ── Headings: ### before ## before # to avoid prefix ambiguity ────────
	if (!line.empty() && line[0] == '#') {
		size_t level = 0;
		while (level < line.size() && line[level] == '#') ++level;
		// Require a space after the hashes.
		if (level < line.size() && line[level] == ' ') {
			const std::string content = line.substr(level + 1);
			Run base;
			base.bold = true;
			switch (level) {
				case 1:
					base.sizeScale = 1.6f;
					base.hasColor  = true;
					base.color     = kColorH1;
					break;
				case 2:
					base.sizeScale = 1.35f;
					base.hasColor  = true;
					base.color     = kColorH2;
					break;
				default:
					base.sizeScale = 1.15f;
					base.hasColor  = true;
					base.color     = kColorH3;
					break;
			}
			// Leading newline for visual separation.
			{
				Run nl;
				nl.text = "\n";
				AppendRun(nl);
			}
			// Pass through RenderInline so **bold** inside headings works.
			RenderInline(content, base);
			{
				Run nl;
				nl.text = "\n";
				AppendRun(nl);
			}
			ScrollToEnd();
			return;
		}
	}

	// ── Blockquote: > ... ─────────────────────────────────────────────────
	if (!line.empty() && line[0] == '>') {
		const std::string content = (line.size() > 1 && line[1] == ' ')
		    ? line.substr(2) : line.substr(1);
		// Indent marker.
		{
			Run r;
			r.text     = "  \xE2\x94\x82 "; // U+2502 │
			r.hasColor = true;
			r.color    = kColorBlockquote;
			AppendRun(r);
		}
		Run base;
		base.italic    = true;
		base.hasColor  = true;
		base.color     = kColorBlockquote;
		RenderInline(content, base);
		Run nl; nl.text = "\n"; AppendRun(nl);
		ScrollToEnd();
		return;
	}

	// ── Unordered list: - / * / + ─────────────────────────────────────────
	if (line.size() >= 2 &&
	    (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
	    line[1] == ' ') {
		const std::string content = line.substr(2);
		{
			Run r;
			r.text     = "  \xE2\x80\xA2 "; // U+2022 •
			r.hasColor = true;
			r.color    = kColorDim;
			AppendRun(r);
		}
		Run base;
		RenderInline(content, base);
		Run nl; nl.text = "\n"; AppendRun(nl);
		ScrollToEnd();
		return;
	}

	// ── Indented unordered list (2–4 spaces then - / * / +) ───────────────
	{
		size_t indent = 0;
		while (indent < line.size() && line[indent] == ' ') ++indent;
		if (indent >= 2 && indent + 1 < line.size() &&
		    (line[indent] == '-' || line[indent] == '*' || line[indent] == '+') &&
		    line[indent + 1] == ' ') {
			const std::string content = line.substr(indent + 2);
			{
				Run r;
				// Extra indent for nested bullets.
				r.text     = std::string(indent + 2, ' ') + "\xE2\x80\xA2 ";
				r.hasColor = true;
				r.color    = kColorDim;
				AppendRun(r);
			}
			Run base;
			RenderInline(content, base);
			Run nl; nl.text = "\n"; AppendRun(nl);
			ScrollToEnd();
			return;
		}
	}

	// ── Ordered list: 1. / 2. / ... ───────────────────────────────────────
	{
		size_t j = 0;
		while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
		if (j > 0 && j < line.size() && line[j] == '.' &&
		    j + 1 < line.size() && line[j + 1] == ' ') {
			const std::string number  = line.substr(0, j + 1) + " ";
			const std::string content = line.substr(j + 2);
			{
				Run r;
				r.text     = "  " + number;
				r.hasColor = true;
				r.color    = kColorDim;
				AppendRun(r);
			}
			Run base;
			RenderInline(content, base);
			Run nl; nl.text = "\n"; AppendRun(nl);
			ScrollToEnd();
			return;
		}
	}

	// ── Plain paragraph line ───────────────────────────────────────────────
	Run base;
	RenderInline(line, base);
	Run nl; nl.text = "\n"; AppendRun(nl);
	ScrollToEnd();
}

// ---------------------------------------------------------------------------
// StripHtml
// ---------------------------------------------------------------------------

std::string StripHtml(const std::string& html)
{
	std::string out;
	out.reserve(html.size());
	bool inTag = false;
	size_t i   = 0;

	while (i < html.size()) {
		if (html[i] == '<') {
			inTag = true;
			// Insert newline before block-level tags for readability.
			const std::string rest = html.substr(i + 1, 10);
			auto startsWithI = [&](const char* tag) -> bool {
				size_t tl = strlen(tag);
				if (rest.size() < tl) return false;
				for (size_t k = 0; k < tl; ++k)
					if (std::tolower(static_cast<unsigned char>(rest[k])) !=
					    std::tolower(static_cast<unsigned char>(tag[k]))) return false;
				return true;
			};
			if (startsWithI("p") || startsWithI("div") ||
			    startsWithI("br") || startsWithI("h1") ||
			    startsWithI("h2") || startsWithI("h3") ||
			    startsWithI("li") || startsWithI("tr")) {
				if (!out.empty() && out.back() != '\n') out += '\n';
			}
			++i;
			continue;
		}
		if (inTag) {
			if (html[i] == '>') inTag = false;
			++i;
			continue;
		}
		// HTML entities.
		if (html[i] == '&') {
			const size_t semi = html.find(';', i + 1);
			if (semi != std::string::npos && semi - i <= 10) {
				const std::string ent = html.substr(i + 1, semi - i - 1);
				if      (ent == "amp")  out += '&';
				else if (ent == "lt")   out += '<';
				else if (ent == "gt")   out += '>';
				else if (ent == "quot") out += '"';
				else if (ent == "apos") out += '\'';
				else if (ent == "nbsp") out += ' ';
				else if (ent == "#160") out += ' ';
				else if (!ent.empty() && ent[0] == '#') {
					// Numeric entity — decode.
					int code = 0;
					if (ent.size() > 1 && ent[1] == 'x')
						code = static_cast<int>(std::strtol(ent.c_str() + 2, nullptr, 16));
					else
						code = std::atoi(ent.c_str() + 1);
					if (code > 0 && code < 128) out += static_cast<char>(code);
					else out += '?';
				} else {
					// Unknown — pass through.
					out += '&'; out += ent; out += ';';
				}
				i = semi + 1;
				continue;
			}
		}
		out += html[i++];
	}

	// Collapse runs of 3+ blank lines to max 2.
	std::string result;
	result.reserve(out.size());
	int blankCount = 0;
	for (char c : out) {
		if (c == '\n') {
			++blankCount;
			if (blankCount <= 2) result += c;
		} else {
			blankCount = 0;
			result += c;
		}
	}
	return result;
}

} // namespace md
