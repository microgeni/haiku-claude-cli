// syntax_highlight.cpp — language-aware tokeniser for GUI code blocks.
//
// Ports the highlight_with() logic from tui.cpp but emits TokenSpan
// structs instead of ANSI escape sequences.  The tokeniser is a
// single-pass scanner; see tui.cpp for the design rationale.
//
// Token priority (first match wins):
//   1. C/C++ preprocessor directive  (whole-line fast-path)
//   2. Line comment  (// or #)
//   3. Block comment  /* … */
//   4. String literal  " … " or ' … '
//   5. Shell variable  $VAR / ${…} / $(…) / $? / $# …
//   6. Numeric literal
//   7. Identifier → keyword / type / builtin / UPPER_CASE / plain
//   8. C++ multi-char operators  (::, ->, <<, >> …)
//   9. Fallthrough character  → plain

#include "syntax_highlight.h"

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace syntax {

// ── Language descriptor ───────────────────────────────────────────────────

struct LangSpec {
	const std::unordered_set<std::string>* keywords   = nullptr;
	const std::unordered_set<std::string>* type_words = nullptr;
	const std::unordered_set<std::string>* builtins   = nullptr;
	bool has_slash_comment  = false; // // line comment
	bool has_hash_comment   = false; // # line comment
	bool has_block_comment  = false; // /* … */
	bool has_single_string  = false; // '…'
	bool has_numbers        = true;
	bool has_preprocessor   = false; // C/C++ # directives
	bool has_operators      = false; // ::, ->, <<, >> …
	bool has_shell_vars     = false; // $VAR / ${VAR}
	bool has_constants      = false; // UPPER_CASE → Constant
};

// ── Keyword / type / builtin tables ──────────────────────────────────────

static const std::unordered_set<std::string>& cpp_keywords()
{
	static const std::unordered_set<std::string> k = {
		"auto","break","case","catch","continue","default","delete","do","else",
		"explicit","extern","for","friend","goto","if","inline","namespace","new",
		"noexcept","operator","private","protected","public","return","sizeof",
		"static","static_cast","dynamic_cast","reinterpret_cast","const_cast",
		"switch","template","this","throw","try","typedef","typename","union",
		"using","virtual","while","nullptr","true","false","class","struct","enum",
		"constexpr","consteval","constinit","co_await","co_return","co_yield",
		"requires","concept","export","import","module","override","final",
		"alignas","alignof","decltype","noreturn","thread_local","static_assert",
		"const","volatile","register","mutable"
	};
	return k;
}

static const std::unordered_set<std::string>& cpp_types()
{
	static const std::unordered_set<std::string> k = {
		"void","bool","char","short","int","long","float","double",
		"signed","unsigned","wchar_t","char8_t","char16_t","char32_t",
		"size_t","ssize_t","ptrdiff_t","intptr_t","uintptr_t",
		"int8_t","int16_t","int32_t","int64_t",
		"uint8_t","uint16_t","uint32_t","uint64_t",
		"int_fast8_t","int_fast16_t","int_fast32_t","int_fast64_t",
		"uint_fast8_t","uint_fast16_t","uint_fast32_t","uint_fast64_t",
		"int_least8_t","int_least16_t","int_least32_t","int_least64_t",
		"uint_least8_t","uint_least16_t","uint_least32_t","uint_least64_t",
		"intmax_t","uintmax_t",
		"std","string","vector","map","unordered_map","set","unordered_set",
		"pair","tuple","optional","variant","any","span","array","deque",
		"list","forward_list","queue","stack","priority_queue",
		"shared_ptr","unique_ptr","weak_ptr","atomic","mutex","thread",
		"ifstream","ofstream","fstream","istringstream","ostringstream",
		"stringstream","ostream","istream","iostream","FILE","DIR"
	};
	return k;
}

static const std::unordered_set<std::string>& py_keywords()
{
	static const std::unordered_set<std::string> k = {
		"False","None","True","and","as","assert","async","await","break",
		"class","continue","def","del","elif","else","except","finally",
		"for","from","global","if","import","in","is","lambda","nonlocal",
		"not","or","pass","raise","return","try","while","with","yield",
		"self","cls"
	};
	return k;
}

static const std::unordered_set<std::string>& sh_keywords()
{
	static const std::unordered_set<std::string> k = {
		"if","then","else","elif","fi","case","esac","for","while","do",
		"done","in","function","select","until","return","break","continue",
		"time","[[","]]"
	};
	return k;
}

static const std::unordered_set<std::string>& sh_builtins()
{
	static const std::unordered_set<std::string> k = {
		"echo","printf","read","local","export","readonly","unset","set",
		"shift","trap","true","false","exit","exec","eval","source",".",
		"test","[",
		"cd","pwd","ls","cat","grep","sed","awk","cut","sort","uniq","wc",
		"head","tail","find","xargs","mkdir","rmdir","rm","cp","mv","ln",
		"touch","chmod","chown","make","cmake","git","curl","wget","tar",
		"gzip","zip","unzip","env","which","type","command","hash","alias",
		"declare","typeset","mapfile","readarray","getopts","wait","jobs",
		"fg","bg","kill","sleep","basename","dirname","realpath","stat",
		"file","diff","patch","install","pkg-config",
		"python","python3","perl","ruby","node","npm","pip"
	};
	return k;
}

static const std::unordered_set<std::string>& rust_keywords()
{
	static const std::unordered_set<std::string> k = {
		"as","async","await","break","const","continue","crate","dyn","else",
		"enum","extern","false","fn","for","if","impl","in","let","loop",
		"match","mod","move","mut","pub","ref","return","self","Self",
		"static","struct","super","trait","true","type","unsafe","use",
		"where","while"
	};
	return k;
}

static const std::unordered_set<std::string>& json_keywords()
{
	static const std::unordered_set<std::string> k = { "true","false","null" };
	return k;
}

// ── Language lookup ───────────────────────────────────────────────────────

static bool lookup_lang(const std::string& lang, LangSpec& spec)
{
	std::string l = lang;
	for (auto& c : l)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	if (l == "cpp" || l == "c++" || l == "cxx" || l == "hpp" || l == "hxx" ||
	    l == "c"   || l == "h") {
		spec.keywords          = &cpp_keywords();
		spec.type_words        = &cpp_types();
		spec.has_slash_comment = true;
		spec.has_block_comment = true;
		spec.has_single_string = true;
		spec.has_preprocessor  = true;
		spec.has_operators     = true;
		spec.has_constants     = true;
		return true;
	}
	if (l == "py" || l == "python") {
		spec.keywords          = &py_keywords();
		spec.has_hash_comment  = true;
		spec.has_single_string = true;
		return true;
	}
	if (l == "sh" || l == "bash" || l == "zsh" || l == "shell") {
		spec.keywords          = &sh_keywords();
		spec.builtins          = &sh_builtins();
		spec.has_hash_comment  = true;
		spec.has_single_string = true;
		spec.has_numbers       = false;
		spec.has_shell_vars    = true;
		return true;
	}
	if (l == "rust" || l == "rs") {
		spec.keywords          = &rust_keywords();
		spec.has_slash_comment = true;
		spec.has_block_comment = true;
		spec.has_operators     = true;
		return true;
	}
	if (l == "json") {
		spec.keywords = &json_keywords();
		return true;
	}
	return false;
}

// ── Tokeniser ─────────────────────────────────────────────────────────────
//
// Internal helper that appends a span to `out`, merging with the previous
// span when the kind is the same (reduces run count for the BTextView).

static void push(std::vector<TokenSpan>& out,
                 const std::string& text,
                 TokenKind kind)
{
	if (text.empty()) return;
	if (!out.empty() && out.back().kind == kind)
		out.back().text += text;
	else
		out.push_back({text, kind});
}

static std::vector<TokenSpan> tokenise_with(const LangSpec& spec,
                                             const std::string& line)
{
	std::vector<TokenSpan> out;
	out.reserve(16);

	// ── C/C++ preprocessor fast-path ─────────────────────────────────────
	// If the first non-whitespace char is '#', the entire line is a
	// preprocessor directive. We still highlight string / angle-bracket
	// includes inside it, but everything else is Preprocessor coloured.
	if (spec.has_preprocessor) {
		size_t k = 0;
		while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
		if (k < line.size() && line[k] == '#') {
			// Leading whitespace (plain).
			if (k > 0) push(out, line.substr(0, k), TokenKind::Plain);

			// Find end of directive word (#include, #define …).
			size_t dstart = k + 1;
			while (dstart < line.size() && line[dstart] == ' ') ++dstart;
			size_t dend = dstart;
			while (dend < line.size() &&
			       std::isalpha(static_cast<unsigned char>(line[dend]))) ++dend;

			// Emit # + directive word as Preprocessor.
			push(out, line.substr(k, dend - k), TokenKind::Preprocessor);

			// Scan the rest of the directive, highlighting string / <file>.
			size_t ri = dend;
			while (ri < line.size()) {
				if (line[ri] == '"') {
					size_t s = ri++;
					while (ri < line.size() && line[ri] != '"') ++ri;
					if (ri < line.size()) ++ri;
					push(out, line.substr(s, ri - s), TokenKind::String);
				} else if (line[ri] == '<') {
					size_t s = ri++;
					while (ri < line.size() && line[ri] != '>') ++ri;
					if (ri < line.size()) ++ri;
					push(out, line.substr(s, ri - s), TokenKind::String);
				} else {
					push(out, std::string(1, line[ri++]), TokenKind::Preprocessor);
				}
			}
			return out;
		}
	}

	size_t i = 0;

	// Helper: does line start with `lit` at position i?
	auto starts_at = [&](const char* lit) {
		const size_t n = std::char_traits<char>::length(lit);
		return i + n <= line.size() && line.compare(i, n, lit) == 0;
	};

	while (i < line.size()) {

		// ── Line comment // ───────────────────────────────────────────────
		if (spec.has_slash_comment && starts_at("//")) {
			push(out, line.substr(i), TokenKind::Comment);
			break;
		}

		// ── Line comment # ────────────────────────────────────────────────
		if (spec.has_hash_comment && line[i] == '#') {
			push(out, line.substr(i), TokenKind::Comment);
			break;
		}

		// ── Block comment /* … */ ─────────────────────────────────────────
		if (spec.has_block_comment && starts_at("/*")) {
			const size_t close = line.find("*/", i + 2);
			if (close == std::string::npos) {
				push(out, line.substr(i), TokenKind::Comment);
				break;
			}
			push(out, line.substr(i, close + 2 - i), TokenKind::Comment);
			i = close + 2;
			continue;
		}

		// ── Double-quoted string ──────────────────────────────────────────
		if (line[i] == '"') {
			const size_t start = i++;
			while (i < line.size() && line[i] != '"') {
				if (line[i] == '\\' && i + 1 < line.size()) ++i;
				++i;
			}
			if (i < line.size()) ++i; // closing "
			push(out, line.substr(start, i - start), TokenKind::String);
			continue;
		}

		// ── Single-quoted string / char literal ───────────────────────────
		if (spec.has_single_string && line[i] == '\'') {
			const size_t start = i++;
			while (i < line.size() && line[i] != '\'') {
				if (line[i] == '\\' && i + 1 < line.size()) ++i;
				++i;
			}
			if (i < line.size()) ++i; // closing '
			push(out, line.substr(start, i - start), TokenKind::String);
			continue;
		}

		// ── Shell variable expansions ─────────────────────────────────────
		if (spec.has_shell_vars && line[i] == '$') {
			if (i + 1 < line.size()) {
				const char next = line[i + 1];

				// Special single-char vars: $? $# $@ $$ $! $- $*
				if (next == '?' || next == '#' || next == '@' || next == '$' ||
				    next == '!' || next == '-' || next == '*') {
					push(out, line.substr(i, 2), TokenKind::Special);
					i += 2;
					continue;
				}
				// Positional: $0..$9
				if (std::isdigit(static_cast<unsigned char>(next))) {
					push(out, line.substr(i, 2), TokenKind::Special);
					i += 2;
					continue;
				}
				// ${VAR}
				if (next == '{') {
					const size_t close = line.find('}', i + 2);
					if (close != std::string::npos) {
						push(out, line.substr(i, close + 1 - i), TokenKind::Variable);
						i = close + 1;
					} else {
						push(out, line.substr(i), TokenKind::Variable);
						i = line.size();
					}
					continue;
				}
				// $( … ) command substitution — colour the $( marker only.
				if (next == '(') {
					push(out, line.substr(i, 2), TokenKind::Variable);
					i += 2;
					continue;
				}
				// $IDENTIFIER
				if (std::isalpha(static_cast<unsigned char>(next)) || next == '_') {
					const size_t start = i++;
					while (i < line.size() &&
					       (std::isalnum(static_cast<unsigned char>(line[i])) ||
					        line[i] == '_'))
						++i;
					push(out, line.substr(start, i - start), TokenKind::Variable);
					continue;
				}
			}
			// Bare $ falls through to plain.
		}

		// ── Numeric literals ──────────────────────────────────────────────
		if (spec.has_numbers &&
		    std::isdigit(static_cast<unsigned char>(line[i]))) {
			const size_t start = i;
			if (line[i] == '0' && i + 1 < line.size() &&
			    (line[i + 1] == 'x' || line[i + 1] == 'X')) {
				i += 2;
				while (i < line.size() &&
				       std::isxdigit(static_cast<unsigned char>(line[i]))) ++i;
			} else if (line[i] == '0' && i + 1 < line.size() &&
			           (line[i + 1] == 'b' || line[i + 1] == 'B')) {
				i += 2;
				while (i < line.size() && (line[i] == '0' || line[i] == '1'))
					++i;
			} else {
				while (i < line.size() &&
				       (std::isalnum(static_cast<unsigned char>(line[i])) ||
				        line[i] == '.' || line[i] == '_')) ++i;
			}
			// Trailing type suffixes: u, l, ul, f, …
			while (i < line.size() &&
			       (line[i] == 'u' || line[i] == 'U' ||
			        line[i] == 'l' || line[i] == 'L' ||
			        line[i] == 'f' || line[i] == 'F')) ++i;
			push(out, line.substr(start, i - start), TokenKind::Number);
			continue;
		}

		// ── Identifiers ───────────────────────────────────────────────────
		if (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_') {
			const size_t start = i;
			while (i < line.size() &&
			       (std::isalnum(static_cast<unsigned char>(line[i])) ||
			        line[i] == '_')) ++i;
			const std::string word = line.substr(start, i - start);

			if (spec.keywords && spec.keywords->count(word)) {
				push(out, word, TokenKind::Keyword);
				continue;
			}
			if (spec.type_words && spec.type_words->count(word)) {
				push(out, word, TokenKind::Type);
				continue;
			}
			if (spec.builtins && spec.builtins->count(word)) {
				push(out, word, TokenKind::Builtin);
				continue;
			}
			// UPPER_CASE → Constant (must have at least one letter).
			if (spec.has_constants && !word.empty()) {
				bool all_upper = true, has_letter = false;
				for (const char c : word) {
					if (c != '_' && !std::isupper(static_cast<unsigned char>(c)) &&
					    !std::isdigit(static_cast<unsigned char>(c)))
						all_upper = false;
					if (std::isalpha(static_cast<unsigned char>(c)))
						has_letter = true;
				}
				if (all_upper && has_letter) {
					push(out, word, TokenKind::Constant);
					continue;
				}
			}
			push(out, word, TokenKind::Plain);
			continue;
		}

		// ── C++ multi-char operators ──────────────────────────────────────
		if (spec.has_operators && i + 1 < line.size()) {
			const char a = line[i], b = line[i + 1];
			if ((a == ':' && b == ':') || (a == '-' && b == '>') ||
			    (a == '<' && b == '<') || (a == '>' && b == '>') ||
			    (a == '&' && b == '&') || (a == '|' && b == '|') ||
			    (a == '=' && b == '=') || (a == '!' && b == '=') ||
			    (a == '<' && b == '=') || (a == '>' && b == '=') ||
			    (a == '+' && b == '=') || (a == '-' && b == '=') ||
			    (a == '*' && b == '=') || (a == '/' && b == '=') ||
			    (a == '%' && b == '=') || (a == '&' && b == '=') ||
			    (a == '|' && b == '=') || (a == '^' && b == '=') ||
			    (a == '+' && b == '+') || (a == '-' && b == '-') ||
			    (a == '.' && b == '*') || (a == '~' && b == '=')) {
				push(out, line.substr(i, 2), TokenKind::Operator);
				i += 2;
				continue;
			}
		}
		if (spec.has_operators) {
			const char a = line[i];
			if (a == '=' || a == '!' || a == '<' || a == '>' ||
			    a == '+' || a == '-' || a == '*' || a == '/' ||
			    a == '%' || a == '&' || a == '|' || a == '^' ||
			    a == '~' || a == '?' || a == ':') {
				push(out, std::string(1, a), TokenKind::Operator);
				++i;
				continue;
			}
		}

		// ── Fallthrough: plain character ──────────────────────────────────
		push(out, std::string(1, line[i++]), TokenKind::Plain);
	}

	return out;
}

// ── Public API ────────────────────────────────────────────────────────────

std::vector<TokenSpan> Tokenise(const std::string& lang, const std::string& line)
{
	LangSpec spec;
	if (!lookup_lang(lang, spec)) {
		// Unknown language — return the whole line as a single Plain span.
		return { { line, TokenKind::Plain } };
	}
	return tokenise_with(spec, line);
}

} // namespace syntax
