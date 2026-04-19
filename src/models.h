#ifndef HAIKU_CLAUDE_CLI_MODELS_H
#define HAIKU_CLAUDE_CLI_MODELS_H

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"

// Model metadata and session-status helpers: listing models via
// GET /v1/models, mapping a model id to its context window and
// per-token pricing, and formatting the compact bottom status row
// that the REPL shows above its input prompt.
namespace models {

using json = nlohmann::json;

struct ModelEntry {
	std::string id;
	std::string display_name;
};

struct PriceEntry {
	double input;
	double output;
};

// Fetch available models via GET /v1/models. Returns sorted by the
// server's default ordering (newest first). Empty on any error.
std::vector<ModelEntry> FetchModels(const config::Auth& auth);

// Pick a reasonable context-window size. Config override wins;
// otherwise the "[1m]" suffix signals Anthropic's 1M-token beta and
// every other model gets the standard 200k window.
int DetectContextWindow(const std::string& model, int override_val);

// Look up per-million-token pricing. Config's "prices" block wins
// over the built-in fallbacks keyed on "opus" / "sonnet" / "haiku".
PriceEntry GetPrice(const std::string& model, const json& config_prices);

// Compact a raw token count into a k/M suffix so it fits on the
// bottom status row without dominating the line.
std::string CompactTokens(int n);

// Compose the fixed-bottom status row for a REPL session:
//   <short_model> · turn N · ↑ 1.2k · ↓ 420 · max 8192    right_label
// Truncates to one terminal row wide.
std::string FormatStatusRow(const std::string& model,
                            int turn_count,
                            int session_input,
                            int session_output,
                            int max_tokens,
                            const std::string& right_label);

// Scan text for a numbered list at line start (`1. foo`, `2. bar`,
// ...) and return at least two consecutive entries so callers can
// render inline-keyboard buttons. Labels are trimmed to ~28 chars.
std::vector<std::pair<std::string, std::string>>
ExtractNumberedOptions(const std::string& text);

} // namespace models

#endif
