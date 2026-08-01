#include "markov.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace markov {

namespace {

// P(event | context) with additive smoothing.
//
//   P = (count + a) / (total + a * V)
//
// where `a` is the smoothing pseudo-count and `V` the number of distinct
// continuations we spread the pseudo-mass over. We use the number of
// continuations actually seen from this context plus one slot for
// "something new" (`seen + 1`), rather than a large fixed vocabulary. This
// keeps a *certain* transition genuinely low-surprise (the failure mode of
// spreading `a` over a big fixed V), while still reserving probability mass
// for a never-before-seen continuation so novelty spikes.
double SmoothedProb(double count, double total, double seen, const Config& cfg) {
	const double a = cfg.smoothing;
	const double V = seen + 1.0;   // observed continuations + one "unseen" slot
	return (count + a) / (total + a * V);
}

} // namespace

std::string MarkovModel::ContextKey() const {
	// Join the rolling context with a separator that will not appear in a
	// normalized event key. Short/empty contexts share the "" cold-start key.
	std::string key;
	for (const auto& e : context_) {
		if (!key.empty()) key += '\x1f';   // ASCII unit separator
		key += e;
	}
	return key;
}

double MarkovModel::SurpriseOf(const std::string& event_key) const {
	const std::string ck = ContextKey();
	auto it = table_.find(ck);
	if (it == table_.end())
		return 0.0;   // never seen this context: cold, treat as unsurprising

	double total = 0.0;
	double count = 0.0;
	for (const auto& kv : it->second) {
		total += kv.second;
		if (kv.first == event_key) count = kv.second;
	}

	// Too little evidence to judge — stay quiet rather than cry wolf.
	if (total < cfg_.min_context_mass)
		return 0.0;

	const double seen = static_cast<double>(it->second.size());
	const double p = SmoothedProb(count, total, seen, cfg_);
	return 1.0 - p;
}

double MarkovModel::Observe(const std::string& event_key) {
	// 1. Score surprise against what we currently know (before learning).
	last_surprise_ = SurpriseOf(event_key);

	// 2. Learn: decay existing counts in this context, then add the new one.
	const std::string ck = ContextKey();
	auto& row = table_[ck];
	if (cfg_.decay < 1.0) {
		for (auto& kv : row) kv.second *= cfg_.decay;
	}
	row[event_key] += 1.0;
	++observations_;

	// 3. Advance the rolling context.
	context_.push_back(event_key);
	while (static_cast<int>(context_.size()) > cfg_.order)
		context_.pop_front();

	return last_surprise_;
}

std::string MarkovModel::Predict(double* out_p) const {
	if (out_p) *out_p = 0.0;
	auto it = table_.find(ContextKey());
	if (it == table_.end()) return {};

	double total = 0.0;
	const std::string* best = nullptr;
	double best_count = -1.0;
	for (const auto& kv : it->second) {
		total += kv.second;
		if (kv.second > best_count) { best_count = kv.second; best = &kv.first; }
	}
	if (!best || total < cfg_.min_context_mass) return {};
	if (out_p) *out_p = SmoothedProb(best_count, total,
	                                 static_cast<double>(it->second.size()), cfg_);
	return *best;
}

json MarkovModel::ToJson() const {
	json j;
	j["version"] = 1;
	j["order"] = cfg_.order;
	j["decay"] = cfg_.decay;
	j["smoothing"] = cfg_.smoothing;
	j["vocab_hint"] = cfg_.vocab_hint;
	j["surprise_threshold"] = cfg_.surprise_threshold;
	j["min_context_mass"] = cfg_.min_context_mass;
	j["observations"] = observations_;

	// Persist the rolling context so a reload continues mid-sequence.
	j["context"] = json::array();
	for (const auto& e : context_) j["context"].push_back(e);

	// table: { context_key: { event_key: count, ... }, ... }
	json t = json::object();
	for (const auto& ctx : table_) {
		json row = json::object();
		for (const auto& kv : ctx.second) row[kv.first] = kv.second;
		t[ctx.first] = std::move(row);
	}
	j["table"] = std::move(t);
	return j;
}

bool MarkovModel::FromJson(const json& j) {
	if (!j.is_object() || !j.contains("table")) return false;

	cfg_.order              = j.value("order", cfg_.order);
	cfg_.decay              = j.value("decay", cfg_.decay);
	cfg_.smoothing          = j.value("smoothing", cfg_.smoothing);
	cfg_.vocab_hint         = j.value("vocab_hint", cfg_.vocab_hint);
	cfg_.surprise_threshold = j.value("surprise_threshold", cfg_.surprise_threshold);
	cfg_.min_context_mass   = j.value("min_context_mass", cfg_.min_context_mass);
	observations_           = j.value("observations", static_cast<std::uint64_t>(0));

	table_.clear();
	for (const auto& ctx : j["table"].items()) {
		auto& row = table_[ctx.key()];
		for (const auto& kv : ctx.value().items())
			row[kv.key()] = kv.value().get<double>();
	}

	context_.clear();
	if (j.contains("context") && j["context"].is_array()) {
		for (const auto& e : j["context"]) context_.push_back(e.get<std::string>());
		while (static_cast<int>(context_.size()) > cfg_.order)
			context_.pop_front();
	}
	return true;
}

bool MarkovModel::Save(const std::string& path) const {
	std::ofstream out(path, std::ios::trunc);
	if (!out) return false;
	out << ToJson().dump(1, '\t');
	return static_cast<bool>(out);
}

bool MarkovModel::Load(const std::string& path) {
	std::ifstream in(path);
	if (!in) return false;
	json j;
	try {
		in >> j;
	} catch (const json::exception&) {
		return false;   // corrupt / partial: caller starts fresh
	}
	return FromJson(j);
}

} // namespace markov
