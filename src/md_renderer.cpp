#include "md_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "sci_output.h"

namespace md {

// ---------------------------------------------------------------------------
// Colour constants (BTextView path)
// ---------------------------------------------------------------------------
namespace {

const rgb_color kColorText       = { 220, 220, 220, 255 };
const rgb_color kColorDim        = { 140, 140, 140, 255 };
const rgb_color kColorCode       = { 200, 200, 160, 255 };
const rgb_color kColorH1         = { 255, 200, 100, 255 };
const rgb_color kColorH2         = { 180, 220, 255, 255 };
const rgb_color kColorH3         = { 200, 255, 200, 255 };
const rgb_color kColorBlockquote = { 150, 150, 150, 255 };
const rgb_color kColorLink       = {  86, 180, 233, 255 };
const rgb_color kColorHRule      = { 100, 100, 100, 255 };

// Map a Run to a SciOutput style index.
int RunToSciStyle(const Run& r)
{
	if (r.hasSciStyle) return r.sciStyle;
	if (r.monospace)   return SciOutput::kStyleCode;
	if (r.hasColor) {
		// Match colours to known SciOutput styles.
		auto eq = [](rgb_color a, rgb_color b) {
			return a.red == b.red && a.green == b.green && a.blue == b.blue;
		};
		if (eq(r.color, kColorH1))         return SciOutput::kStyleH1;
		if (eq(r.color, kColorH2))         return SciOutput::kStyleH2;
		if (eq(r.color, kColorH3))         return SciOutput::kStyleH3;
		if (eq(r.color, kColorLink))       return SciOutput::kStyleLink;
		if (eq(r.color, kColorBlockquote)) return SciOutput::kStyleBlockquote;
		if (eq(r.color, kColorDim))        return SciOutput::kStyleDim;
		if (eq(r.color, kColorHRule))      return SciOutput::kStyleDim;
	}
	if (r.italic) return SciOutput::kStyleBlockquote;
	return SciOutput::kStyleDefault;
}

} // namespace

// ---------------------------------------------------------------------------
// MdRenderer
// ---------------------------------------------------------------------------

MdRenderer::MdRenderer(BTextView* view)
	: fView(view)
{
}

MdRenderer::MdRenderer(SciOutput* output)
	: fSciOutput(output)
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
	if (!fLineBuf.empty()) {
		RenderLine(fLineBuf);
		fLineBuf.clear();
	}
}

// ---------------------------------------------------------------------------
// AppendRun — emit a styled text run to BTextView
// ---------------------------------------------------------------------------

void MdRenderer::AppendRun(const Run& r)
{
	if (r.text.empty()) return;

	// ── SciOutput backend ────────────────────────────────────────────────────
	if (fSciOutput) {
		fSciOutput->AppendText(r.text, RunToSciStyle(r));
		return;
	}

	// ── BTextView backend ────────────────────────────────────────────────────
	if (!fView) return;

	BFont font(r.monospace ? be_fixed_font : be_plain_font);
	if (r.sizeScale != 1.0f)
		font.SetSize(font.Size() * r.sizeScale);
	uint16 face = B_REGULAR_FACE;
	if (r.bold)      face |= B_BOLD_FACE;
	if (r.italic)    face |= B_ITALIC_FACE;
	if (r.underline) face |= B_UNDERSCORE_FACE;
	if (face != B_REGULAR_FACE) font.SetFace(face);

	rgb_color color = kColorText;
	if (r.hasColor)   color = r.color;
	else if (r.monospace) color = kColorCode;

	text_run_array* tra = static_cast<text_run_array*>(
		malloc(sizeof(text_run_array) + sizeof(text_run)));
	if (!tra) {
		fView->Insert(r.text.c_str(), static_cast<int32>(r.text.size()));
		return;
	}
	tra->count          = 1;
	tra->runs[0].offset = 0;
	tra->runs[0].font   = font;
	tra->runs[0].color  = color;
	const int32 start   = fView->TextLength();
	fView->Insert(start, r.text.c_str(), static_cast<int32>(r.text.size()), tra);
	free(tra);
}

void MdRenderer::ScrollToEnd()
{
	if (fSciOutput) { fSciOutput->ScrollToEnd(); return; }
	if (fView)      fView->ScrollToOffset(fView->TextLength());
}

// ---------------------------------------------------------------------------
// AppendHRule
// ---------------------------------------------------------------------------

void MdRenderer::AppendHRule()
{
	// Width estimate: 60 chars for SciOutput (variable-width unknown),
	// or derive from BTextView bounds.
	int nchars = 60;
	if (fView) nchars = std::max(10, static_cast<int>(fView->Bounds().Width() / 8.0f));

	std::string rule;
	rule.reserve(static_cast<size_t>(nchars) * 3 + 1);
	for (int i = 0; i < nchars; ++i) rule += "\xE2\x94\x80"; // U+2500 ─
	rule += '\n';

	Run r;
	r.text     = rule;
	r.hasColor = true;
	r.color    = kColorHRule;
	r.sciStyle    = SciOutput::kStyleDim;
	r.hasSciStyle = true;
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

	// ── Headings: # / ## / ### ─────────────────────────────────────────────
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
			// Emit a leading newline for visual separation.
			{
				Run nl;
				nl.text = "\n";
				AppendRun(nl);
			}
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
