#include "md_text.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace md {

// A line is a table row if it contains at least one '|' and, when trimmed,
// starts with '|' or has an embedded " | ".
bool IsTableRow(const std::string& line)
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
bool IsSeparatorRow(const std::string& line)
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
std::vector<std::string> SplitCells(const std::string& line)
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

// Strip HTML tags and decode common entities from a string.
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
