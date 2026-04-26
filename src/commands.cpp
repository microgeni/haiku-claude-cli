#include "commands.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

#include "api.h"
#include "models.h"
#include "paths.h"
#include "stats.h"
#include "tools.h"
#include "tui.h"

namespace commands {

namespace {

std::unordered_map<std::string, std::string> g_commands;

std::string slurp(const std::string& path) {
	std::ifstream f(path);
	if (!f.is_open()) return {};
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

bool ends_with(const std::string& s, const std::string& suffix) {
	if (suffix.size() > s.size()) return false;
	return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void load_dir(const std::string& dir) {
	DIR* d = opendir(dir.c_str());
	if (!d) return;
	const struct dirent* ent;
	while ((ent = readdir(d)) != nullptr) {
		const std::string name = ent->d_name;
		if (!ends_with(name, ".md")) continue;
		const std::string cmd_name = name.substr(0, name.size() - 3);
		std::string body = slurp(dir + "/" + name);
		// Trim trailing newline so substituted prompts stay tidy.
		while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
			body.pop_back();
		}
		if (body.empty()) continue;
		g_commands[cmd_name] = std::move(body);
	}
	closedir(d);
}

std::string substitute(const std::string& body, const std::string& args) {
	std::string out;
	out.reserve(body.size() + args.size());
	const std::string marker = "{{args}}";
	size_t i = 0;
	while (i < body.size()) {
		if (i + marker.size() <= body.size()
			&& body.compare(i, marker.size(), marker) == 0) {
			out += args;
			i += marker.size();
		} else {
			out += body[i++];
		}
	}
	return out;
}

} // namespace

void Load(const std::string& user_dir) {
	g_commands.clear();
	// User directory first; project Definitions overwrite on collision.
	load_dir(user_dir);
	load_dir(".claude/commands");
}

std::vector<std::string> Names() {
	std::vector<std::string> out;
	out.reserve(g_commands.size());
	for (const auto& entry : g_commands) out.push_back(entry.first);
	std::sort(out.begin(), out.end());
	return out;
}

std::optional<std::string> Expand(const std::string& name, const std::string& args) {
	const auto it = g_commands.find(name);
	if (it == g_commands.end()) return std::nullopt;
	return substitute(it->second, args);
}

SlashAction Dispatch(const std::string& line, LoopCtx& ctx,
					 std::string& passthrough_out) {
	using nlohmann::json;

	std::string cmd = line;
	std::string args;
	if (const auto sp = line.find(' '); sp != std::string::npos) {
		cmd  = line.substr(0, sp);
		args = line.substr(sp + 1);
		while (!args.empty() && args.front() == ' ') args.erase(args.begin());
	}

	if (cmd == "/help" || cmd == "/?") {
		std::cout << tui::Meta(
			"multi-line input:\n"
			"  \\ + Enter          new line (works everywhere, including SSH)\n"
			"  Ctrl+J             new line (local terminal)\n"
			"  Alt+Enter          new line (local terminal)\n"
			"\n"
			"slash commands:\n"
			"  /help              this list\n"
			"  /clear             reset the running conversation\n"
			"  /model [name]      list all available models, or swap to <name>\n"
			"  /compact           summarize and replace the running history\n"
			"  /usage             session tokens, cost estimate, subscription windows\n"
			"  /todos             show the current in-session todo list\n"
			"  /memory [user]     open CLAUDE.md in $EDITOR (project by default)\n"
			"  /stats             lifetime token usage and tool stats\n"
			"  /open [N|URL]      list URLs from this session, open #N, or open URL\n"
			"  /notify [on|off|S] desktop notification on slow turns (default 60s)\n"
			"  /remote-control    toggle Telegram remote control on/off\n"
			"  /ludicrous         toggle ludicrous mode (auto-approve all tool permissions)\n"
			"  /exit, /quit       leave the REPL (Ctrl+D also works)\n")
				  << "\n";
		const auto custom = Names();
		if (!custom.empty()) {
			std::string body = "custom commands from .claude/commands/ and user dir:\n";
			for (const auto& c : custom) body += "  /" + c + "\n";
			std::cout << tui::Meta(body) << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/todos") {
		const auto result = tools::Run("TodoRead", json::object());
		std::cout << tui::Meta("current todos:") << "\n"
				  << result.content << "\n";
		return SlashAction::Continue;
	}
	if (cmd == "/stats") {
		std::cout << tui::Meta(stats::FormatDisplay()) << "\n";
		return SlashAction::Continue;
	}
	if (cmd == "/memory") {
		// With an explicit "user" arg go straight there; otherwise
		// show a picker so the user can choose project or user scope.
		std::string target;
		if (args == "user") {
			target = paths::UserMemoryPath();
			const auto slash = target.rfind('/');
			if (slash != std::string::npos) paths::MkdirP(target.substr(0, slash));
		} else if (!args.empty()) {
			// Any other explicit arg is treated as a direct path (future-proofing).
			target = paths::ProjectMemoryPath();
		} else {
			const std::string proj = paths::ProjectMemoryPath();
			const std::string user = paths::UserMemoryPath();
			const std::vector<std::string> options = {
				"Project  (" + proj + ")",
				"User     (" + user + ")",
			};
			const int picked = tui::SelectOption(options, "open memory file:");
			if (picked == 1) {
				target = user;
				const auto slash = target.rfind('/');
				if (slash != std::string::npos) paths::MkdirP(target.substr(0, slash));
			} else {
				target = proj;
			}
		}
		const char* editor_env = std::getenv("EDITOR");  // flawfinder: ignore
		const std::string editor = editor_env && *editor_env ? editor_env : "nano";
		const std::string cmdline = editor + " '" + target + "'";
		std::cout << tui::Meta("[opening " + target + " with " + editor + "]") << "\n";
		const int rc = std::system(cmdline.c_str());  // flawfinder: ignore
		if (rc != 0) {
			std::cout << tui::Meta("[editor exited " + std::to_string(rc) + "]") << "\n";
		} else {
			std::cout << tui::Meta("[memory will be reloaded on the next turn]") << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/notify") {
		auto state_line = [&]() {
			char buf[128];
			std::snprintf(buf, sizeof(buf),
				"[notify: %s, threshold %.0fs]",
				ctx.notify_enabled ? "on" : "off",
				ctx.notify_min_duration);
			return std::string(buf);
		};
		if (args.empty()) {
			std::cout << tui::Meta(state_line()) << "\n";
			std::cout << tui::Dim("  /notify on | off | <seconds>") << "\n";
			return SlashAction::Continue;
		}
		if (args == "on")  { ctx.notify_enabled = true;  std::cout << tui::Meta(state_line()) << "\n"; return SlashAction::Continue; }
		if (args == "off") { ctx.notify_enabled = false; std::cout << tui::Meta(state_line()) << "\n"; return SlashAction::Continue; }

		char* end = nullptr;
		const double v = std::strtod(args.c_str(), &end);
		if (end == args.c_str() || *end != '\0') {
			std::cout << tui::Meta("[/notify: expected 'on', 'off', or a number of seconds]") << "\n";
			return SlashAction::Continue;
		}
		if (v < 0.0) {
			std::cout << tui::Meta("[/notify: threshold must be >= 0]") << "\n";
			return SlashAction::Continue;
		}
		ctx.notify_min_duration = v;
		std::cout << tui::Meta(state_line()) << "\n";
		return SlashAction::Continue;
	}
	if (cmd == "/open") {
		std::string target;
		if (args.empty()) {
			if (ctx.session_urls.empty()) {
				std::cout << tui::Meta("[no URLs seen in this session yet]") << "\n";
				return SlashAction::Continue;
			}
			for (size_t i = 0; i < ctx.session_urls.size(); ++i) {
				const std::string idx = "  " + std::to_string(i + 1) + ". ";
				std::cout << tui::Meta(idx + ctx.session_urls[i]) << "\n";
			}
			std::cout << tui::Dim("  /open N to launch, or /open <url>") << "\n";
			return SlashAction::Continue;
		}

		bool is_num = !args.empty();
		for (char c : args) {
			if (!std::isdigit(static_cast<unsigned char>(c))) { is_num = false; break; }
		}
		if (is_num) {
			const size_t idx = static_cast<size_t>(std::atoi(args.c_str()));
			if (idx == 0 || idx > ctx.session_urls.size()) {
				std::cout << tui::Meta("[no URL #" + args + " — /open with no args to list]") << "\n";
				return SlashAction::Continue;
			}
			target = ctx.session_urls[idx - 1];
		} else {
			target = args;
		}

		// Fire-and-forget via `open`. Background the child so the
		// REPL keeps its status frame; redirect stdout/stderr so
		// the launcher can't stomp on our TUI.
		const std::string cmdline =
			"open " + config::ShellSingleQuote(target) + " >/dev/null 2>&1 &";
		std::cout << tui::Meta("[opening " + target + "]") << "\n";
		const int rc = std::system(cmdline.c_str());  // flawfinder: ignore
		if (rc != 0) {
			std::cout << tui::Meta("[open exited " + std::to_string(rc) + "]") << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/exit" || cmd == "/quit") {
		return SlashAction::Quit;
	}
	if (cmd == "/ludicrous") {
		const bool now = !api::g_ludicrous_mode.load();
		api::g_ludicrous_mode.store(now);
		if (now) {
			std::cout << tui::Yellow("\xE2\x9A\xA1 LUDICROUS MODE ENGAGED")
					  << tui::Dim(" \xe2\x80\x94 all tool permissions auto-approved") << "\n";
		} else {
			std::cout << tui::Dim("\xE2\x9A\xA1 Ludicrous mode off \xe2\x80\x94 permission prompts restored") << "\n";
		}
		if (ctx.redraw_status) ctx.redraw_status();
		return SlashAction::Continue;
	}
	if (cmd == "/clear") {
		ctx.messages        = json::array();
		ctx.turn_count      = 0;
		ctx.session_input   = 0;
		ctx.session_output  = 0;
		std::cout << tui::Meta("[conversation cleared]") << "\n";
		if (ctx.redraw_status) ctx.redraw_status();
		return SlashAction::Continue;
	}
	if (cmd == "/model") {
		if (args.empty()) {
			const auto avail = models::FetchModels(ctx.auth);
			if (avail.empty()) {
				std::cout << tui::Meta("[current model: " + ctx.model + "]") << "\n";
				std::cout << tui::Meta("[could not fetch model list — check connection/key]") << "\n";
				return SlashAction::Continue;
			}
			std::vector<std::string> options;
			options.reserve(avail.size());
			int preselect = 0;
			for (int i = 0; i < static_cast<int>(avail.size()); ++i) {
				const auto& m = avail[i];
				std::string label = m.id;
				if (m.display_name != m.id)
					label += "  (" + m.display_name + ")";
				options.push_back(std::move(label));
				if (m.id == ctx.model) preselect = i;
			}
			// Rotate so the currently-active model is highlighted first.
			std::vector<std::string> ordered = options;
			std::vector<int>         index_map(avail.size());
			std::iota(index_map.begin(), index_map.end(), 0);
			if (preselect > 0) {
				std::rotate(ordered.begin(),
							ordered.begin() + preselect,
							ordered.end());
				std::rotate(index_map.begin(),
							index_map.begin() + preselect,
							index_map.end());
			}
			const int picked = tui::SelectOption(ordered, "select model:");
			const int real_idx = index_map[picked];
			const std::string chosen = avail[real_idx].id;
			if (chosen != ctx.model) {
				ctx.model = chosen;
				std::cout << tui::Meta("[model set to " + ctx.model + "]") << "\n";
				if (ctx.redraw_status) ctx.redraw_status();
			} else {
				std::cout << tui::Meta("[model unchanged: " + ctx.model + "]") << "\n";
			}
		} else {
			ctx.model = args;
			std::cout << tui::Meta("[model set to " + ctx.model + "]") << "\n";
			if (ctx.redraw_status) ctx.redraw_status();
		}
		return SlashAction::Continue;
	}
	if (cmd == "/usage") {
		auto header = [](const std::string& key) -> std::string {
			const auto it = api::g_last_rate_headers.find(key);
			return it == api::g_last_rate_headers.end() ? std::string() : it->second;
		};

		auto render_bar = [](double pct) {
			constexpr int kBarWidth = 50;
			if (pct < 0.0)   pct = 0.0;
			if (pct > 100.0) pct = 100.0;
			const int filled = static_cast<int>(pct * kBarWidth / 100.0 + 0.5);
			std::string out;
			for (int i = 0; i < filled;                ++i) out += "\u2588";
			for (int i = 0; i < kBarWidth - filled;    ++i) out += ' ';
			return out;
		};

		auto format_reset = [](const std::string& ts_str) {
			if (ts_str.empty()) return std::string();
			const time_t ts = static_cast<time_t>(std::atoll(ts_str.c_str()));
			std::tm tm {};
			localtime_r(&ts, &tm);
			char out[64];
			std::strftime(out, sizeof(out), "%a %b %d at %H:%M (%Z)", &tm);
			return std::string(out);
		};

		auto print_window = [&](const std::string& label,
								const std::string& util_key,
								const std::string& reset_key) {
			const std::string util_s  = header(util_key);
			const std::string reset_s = header(reset_key);
			if (util_s.empty()) return;
			const double util = std::atof(util_s.c_str());
			const double pct  = util * 100.0;
			char pct_str[16];
			std::snprintf(pct_str, sizeof(pct_str), "%3.0f%% used", pct);
			std::cout << "  " << tui::Bold(label) << "\n"
					  << "  " << render_bar(pct) << " " << pct_str << "\n"
					  << "  " << tui::Dim("Resets " + format_reset(reset_s)) << "\n"
					  << "\n";
		};

		// Session summary (our own state).
		const models::PriceEntry price = models::GetPrice(ctx.model, ctx.prices);
		const double in_cost   = (ctx.session_input  / 1'000'000.0) * price.input;
		const double out_cost  = (ctx.session_output / 1'000'000.0) * price.output;
		char session_buf[512];
		std::snprintf(session_buf, sizeof(session_buf),
			"  model %s  turns %d  in %d  out %d  est $%.4f",
			ctx.model.c_str(),
			ctx.turn_count,
			ctx.session_input,
			ctx.session_output,
			in_cost + out_cost);
		std::cout << tui::Dim(session_buf) << "\n\n";

		if (header("anthropic-ratelimit-unified-5h-utilization").empty()) {
			std::cout << tui::Dim("(no rate-limit data yet — make a request first)")
					  << "\n";
			return SlashAction::Continue;
		}

		print_window("Current session",
					 "anthropic-ratelimit-unified-5h-utilization",
					 "anthropic-ratelimit-unified-5h-reset");
		print_window("Current week (all models)",
					 "anthropic-ratelimit-unified-7d-utilization",
					 "anthropic-ratelimit-unified-7d-reset");
		print_window("Current week (Sonnet only)",
					 "anthropic-ratelimit-unified-7d_sonnet-utilization",
					 "anthropic-ratelimit-unified-7d_sonnet-reset");

		const std::string claim = header("anthropic-ratelimit-unified-representative-claim");
		if (!claim.empty()) {
			std::cout << tui::Dim("  binding window: " + claim) << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/compact") {
		if (ctx.messages.empty()) {
			std::cout << tui::Meta("[nothing to compact]") << "\n";
			return SlashAction::Continue;
		}
		// Confirmation prompt — skipped when auto-compact triggered
		// this call automatically.
		if (!ctx.auto_compact) {
			const std::vector<std::string> options = {
				"Yes, summarize and replace history",
				"No, keep history as-is",
			};
			const int picked = tui::SelectOption(options, "compact conversation history?");
			if (picked != 0) {
				std::cout << tui::Meta("[compact cancelled]") << "\n";
				return SlashAction::Continue;
			}
		}
		nlohmann::json request_messages = ctx.messages;
		request_messages.push_back({
			{"role",    "user"},
			{"content",
				"Two tasks, in order:\n"
				"\n"
				"1. For each source file you've gained real understanding "
				"of during this conversation, call WriteAttr to persist a "
				"concise one-line claude:summary capturing what the file "
				"is for. Only write for files you could confidently "
				"describe — skip files only mentioned in passing. "
				"WriteAttr is auto-approved and restricted to the "
				"claude:* namespace, so these writes are cheap and safe. "
				"This lets future sessions start with accurate summaries "
				"instead of the mechanical auto-seed placeholders.\n"
				"\n"
				"2. Then summarize the preceding conversation in 2-3 "
				"short paragraphs, preserving important context, "
				"decisions, code, and open questions. Reply with only "
				"the summary after the WriteAttr calls."},
		});
		std::cout << "\n" << tui::ClaudePrompt();
		const std::string compact_system = config::ComposeSystem(ctx.custom_system);
		// SendWithTools (not SendConversation) so the WriteAttr
		// calls inside step 1 actually fire.
		const auto result = api::SendWithTools(
			ctx.auth, ctx.model, ctx.max_tokens,
			request_messages, compact_system);
		std::cout << "\n";
		if (result.exit_code != 0) {
			std::cout << tui::Meta("[compact failed]") << "\n";
			return SlashAction::Continue;
		}
		ctx.session_input  += result.input_tokens;
		ctx.session_output += result.output_tokens;
		ctx.messages = nlohmann::json::array({
			{{"role", "user"},      {"content", "[previous conversation context follows]"}},
			{{"role", "assistant"}, {"content", result.assistant_text}},
		});
		char note[96];
		std::snprintf(note, sizeof(note),
			"[compacted: %d in / %d out tokens]",
			result.input_tokens, result.output_tokens);
		std::cout << tui::Meta(note) << "\n";
		// Persist the compacted history so the next --resume or
		// crash-recovery loads the pruned state.
		config::SaveHistory(ctx.messages, ctx.model, ctx.resume_name);
		// Full BFS snapshot re-scan: compaction may have triggered
		// WriteAttr calls; reload so the next turn's system prompt
		// reflects all summaries written this session.
		config::ReloadBfsSummaries();
		if (ctx.redraw_status) ctx.redraw_status();
		return SlashAction::Continue;
	}
	// Fall back to user-defined commands loaded from
	// .claude/commands/*.md. If a match exists we substitute
	// {{args}} and hand the expanded text back to the REPL loop.
	const std::string cmd_name = cmd.substr(1); // drop leading '/'
	if (auto expanded = Expand(cmd_name, args); expanded) {
		passthrough_out = std::move(*expanded);
		return SlashAction::Passthrough;
	}

	std::cout << tui::Meta("[unknown command: " + cmd + " — try /help]") << "\n";
	return SlashAction::Continue;
}

} // namespace commands
