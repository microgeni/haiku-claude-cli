#ifndef HAIKU_CLAUDE_CLI_MARKOV_H
#define HAIKU_CLAUDE_CLI_MARKOV_H

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

// MarkovModel — a tiny, dependency-free "workflow memory" for the CLI.
//
// It is a lightweight alternative to a full HTM / Numenta cortex (see
// docs elsewhere). Instead of Sparse Distributed Representations and a
// Temporal Memory, it just counts "what event usually follows what" over
// the stream of tool events, and reports how *surprising* each new event
// is given the recent context.
//
//   learning  = increment a transition count
//   prediction = the most likely next event given recent context
//   surprise  = 1 - P(event | recent context)   (the anomaly signal)
//
// Properties, all self-contained (no HTM, no Eigen, no SQLite, no AGPL):
//   * n-gram context   — key on the last N events, not just the previous one,
//                        so it captures short motifs like edit->run->fail->edit.
//   * exponential decay — old counts fade, so the model adapts as habits change
//                        (recovers HTM's "continuous online adaptation").
//   * additive smoothing — an unseen-but-fine event is not infinitely surprising,
//                        which avoids false-alarm spam.
//   * JSON persistence  — serialize/load the count table per repo.
//
// Typical use from a PostToolUse hook:
//
//   markov::MarkovModel m;
//   m.Load(path);                 // per-repo state (ok if missing)
//   double s = m.Observe(key);    // learn + return surprise of this event
//   if (s > m.threshold()) { ...raise a "this is unusual" nudge... }
//   m.Save(path);
//
// `key` is a canonical, normalized string for the event, e.g.
//   "edit:src/auth.py"  /  "run:test:fail"  /  "error:AssertionError@validate_token".
// Normalization (stripping line numbers, building an error signature) is the
// caller's job — see MakeKey() helpers if you add them.
namespace markov {

using json = nlohmann::json;

struct Config {
	// How many preceding events form the context key. 1 = classic first-order
	// Markov; 2-3 captures short workflow motifs. Higher orders get sparse.
	int order = 2;

	// Per-observation multiplier applied to *existing* counts before the new
	// count is added. 1.0 = never forget; <1.0 = exponential decay toward
	// recent behaviour. 0.999 fades over ~thousands of events.
	double decay = 0.999;

	// Additive (Laplace) smoothing. A pseudo-count spread over the continuations
	// actually seen from a context (plus one "unseen" slot), so a first-time
	// event is "surprising but not infinite" while a *certain* transition still
	// scores near-zero surprise. Small values (~0.1) are gentle; larger values
	// make the model quicker to cry "novel".
	double smoothing = 0.1;

	// Legacy / reserved: an over-estimate of the total event vocabulary. No
	// longer used by the surprise math (smoothing now self-adapts to each
	// context's observed continuations), but kept for forward compatibility
	// and diagnostics.
	int vocab_hint = 256;

	// Surprise >= this is considered a candidate anomaly worth acting on.
	// Surprise is in [0,1); tune against logged nudge hit-rate.
	double surprise_threshold = 0.85;

	// Below this many total observations from a context, predictions/surprise
	// are treated as low-confidence (the model has not seen enough yet).
	double min_context_mass = 3.0;
};

class MarkovModel {
public:
	MarkovModel() = default;
	explicit MarkovModel(Config cfg) : cfg_(cfg) {}

	// Learn from `event_key` given the current rolling context, advance the
	// context, and return the *surprise* of this event in [0,1):
	//   0   => fully expected (or first ever event / cold context)
	//   ~1  => never seen following this context
	// The returned value is computed BEFORE the count is incremented, so it
	// reflects how surprising the event was given what the model knew.
	double Observe(const std::string& event_key);

	// Surprise of `event_key` given the current context WITHOUT learning it
	// or advancing the context. Useful for "what-if" / prediction checks.
	double SurpriseOf(const std::string& event_key) const;

	// Most likely next event given the current context, or "" if the context
	// is unknown / too low-mass. Also returns its probability via `out_p`.
	std::string Predict(double* out_p = nullptr) const;

	// Was the last Observe() above the anomaly threshold?
	bool LastWasAnomaly() const { return last_surprise_ >= cfg_.surprise_threshold; }
	double last_surprise() const { return last_surprise_; }
	double threshold() const { return cfg_.surprise_threshold; }

	// Reset only the rolling context (e.g. at a session boundary) while
	// keeping everything the model has learned.
	void ResetContext() { context_.clear(); }

	// Total number of transitions observed (post-decay mass is not counted;
	// this is a raw event counter for diagnostics).
	std::uint64_t observations() const { return observations_; }
	std::size_t contexts() const { return table_.size(); }

	// Persistence. Save() writes pretty JSON; Load() returns false if the
	// file is missing or unparseable (caller should treat that as a fresh
	// model). Both are cheap for a workflow-sized table.
	bool Save(const std::string& path) const;
	bool Load(const std::string& path);

	// In-memory (de)serialization if you prefer to persist via BFS attrs.
	json ToJson() const;
	bool FromJson(const json& j);

	const Config& config() const { return cfg_; }

private:
	// Build the string key for the current rolling context (last `order`
	// events joined). An empty / short context yields a shorter key, which is
	// fine — early events just share a common "cold start" context.
	std::string ContextKey() const;

	// counts[context_key][event_key] = decayed occurrence count.
	std::unordered_map<std::string, std::unordered_map<std::string, double>> table_;

	std::deque<std::string> context_;   // rolling last-N events
	Config cfg_;
	std::uint64_t observations_ = 0;
	double last_surprise_ = 0.0;
};

} // namespace markov

#endif
