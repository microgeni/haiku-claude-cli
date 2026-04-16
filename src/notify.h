#ifndef HAIKU_CLAUDE_CLI_NOTIFY_H
#define HAIKU_CLAUDE_CLI_NOTIFY_H

#include <string>
#include <vector>

// Desktop notifications + small helpers that support them. The
// interactive loop uses these to pop a Haiku notification bubble
// after a long turn completes:
//
//   const auto urls = notify::extract_urls(reply);
//   notify::send(notify::pick_playful_title(elapsed),
//                notify::first_sentence(reply, 120));
//
// On non-Haiku builds `send()` is a no-op so macOS dev under nix
// stays silent. `extract_urls` is also used by /open (REPL slash
// command) — it's here because it was introduced alongside the
// notification work and the two share no other natural home.
namespace notify {

// Scan free-form text for http:// / https:// URLs. Returns the
// matches in order of appearance; trailing punctuation and quote
// characters are stripped. URLs shorter than 11 chars (e.g. just
// "http://a/") are filtered out as likely noise.
std::vector<std::string> extract_urls(const std::string& text);

// Compact a response into something a notification body can show:
// whitespace-collapsed, cut at the first sentence boundary when
// one falls within max_chars, otherwise hard-truncated with an
// ellipsis. Empty input yields "(empty response)".
std::string first_sentence(const std::string& text, size_t max_chars);

// Pick a playful past-tense title like "Pondering complete (24s)".
// Pairs thematically with the spinner's gerund verbs. Cycles
// deterministically by current time-of-day so consecutive turns
// don't all say the same thing.
std::string pick_playful_title(double elapsed_seconds);

// Fire a desktop notification. No-op on non-Haiku builds.
// Runs Haiku's `notify` CLI as a short-lived detached child so
// the REPL's termios + scroll-region state doesn't leak into the
// notification_server dispatch.
void send(const std::string& title, const std::string& body);

} // namespace notify

#endif
