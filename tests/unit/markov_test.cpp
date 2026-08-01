// Unit tests for markov::MarkovModel — the dependency-free workflow-memory
// model (a lightweight alternative to a full HTM cortex). These verify the
// behavioural contract the design relies on:
//
//   * a familiar (deterministic) sequence settles to LOW surprise,
//   * a genuinely novel event spikes to HIGH surprise and flags an anomaly,
//   * an ambiguous continuation reports honest mid-range surprise,
//   * prediction returns the most likely next event,
//   * decay lets the model adapt as habits change,
//   * save/load (and to/from JSON) round-trip the learned state.
//
// Build: see the `test-unit` target in the top-level Makefile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/markov.h"

#include <cstdio>
#include <string>
#include <vector>

using markov::Config;
using markov::MarkovModel;

namespace {

// Feed a fixed sequence once, returning the average surprise across it.
double FeedLoop(MarkovModel& m, const std::vector<std::string>& seq) {
	double sum = 0.0;
	for (const auto& e : seq) sum += m.Observe(e);
	return sum / static_cast<double>(seq.size());
}

} // namespace

TEST_CASE("deterministic sequence settles to low surprise") {
	Config cfg; cfg.order = 2;
	MarkovModel m(cfg);

	const std::vector<std::string> loop = {"a", "b", "c", "d"};

	double first = FeedLoop(m, loop);
	double settled = 0.0;
	for (int rep = 0; rep < 20; ++rep) settled = FeedLoop(m, loop);

	// First exposure is cold (unknown contexts return 0 surprise), so we
	// assert the *settled* value is genuinely low in absolute terms.
	CHECK(settled < 0.25);
	// And once learned, a deterministic loop is not an anomaly.
	CHECK(m.LastWasAnomaly() == false);
	(void)first;
}

TEST_CASE("novel event spikes and flags an anomaly") {
	Config cfg; cfg.order = 2;
	MarkovModel m(cfg);

	const std::vector<std::string> loop = {"a", "b", "c", "d"};
	for (int rep = 0; rep < 20; ++rep) FeedLoop(m, loop);

	double s = m.Observe("ZZZ-never-seen");
	CHECK(s > 0.8);
	CHECK(m.LastWasAnomaly() == true);
}

TEST_CASE("ambiguous continuation reports honest mid-range surprise") {
	Config cfg; cfg.order = 2;
	MarkovModel m(cfg);

	// After "x,y" the next event is 50/50 between "p" and "q".
	for (int rep = 0; rep < 40; ++rep) {
		m.ResetContext();
		m.Observe("x"); m.Observe("y");
		m.Observe((rep % 2 == 0) ? "p" : "q");
	}

	// Query the surprise of one branch given the ambiguous context.
	m.ResetContext();
	m.Observe("x"); m.Observe("y");
	double s = m.SurpriseOf("p");
	// Roughly 1 - 0.5: clearly neither certain (0) nor novel (~1).
	CHECK(s > 0.25);
	CHECK(s < 0.75);
}

TEST_CASE("predicts the most likely next event") {
	Config cfg; cfg.order = 1;
	MarkovModel m(cfg);

	// "a" is followed by "b" most of the time.
	for (int i = 0; i < 10; ++i) { m.ResetContext(); m.Observe("a"); m.Observe("b"); }
	for (int i = 0; i < 2;  ++i) { m.ResetContext(); m.Observe("a"); m.Observe("c"); }

	m.ResetContext();
	m.Observe("a");
	double p = 0.0;
	std::string pred = m.Predict(&p);
	CHECK(pred == "b");
	CHECK(p > 0.5);
}

TEST_CASE("low-mass context stays quiet (no crying wolf)") {
	Config cfg; cfg.order = 1; cfg.min_context_mass = 5.0;
	MarkovModel m(cfg);

	// Only a couple of observations from context "a" — below min_context_mass.
	m.Observe("a"); m.Observe("b");
	m.ResetContext();
	m.Observe("a");
	// Not enough evidence: surprise of anything is suppressed to 0.
	CHECK(m.SurpriseOf("totally-new") == doctest::Approx(0.0));
	CHECK(m.Predict() == "");
}

TEST_CASE("decay lets the model adapt to changed habits") {
	Config cfg; cfg.order = 1; cfg.decay = 0.9;  // fast decay for a short test
	MarkovModel m(cfg);

	// Old habit: a -> b.
	for (int i = 0; i < 30; ++i) { m.ResetContext(); m.Observe("a"); m.Observe("b"); }
	// New habit takes over: a -> c.
	for (int i = 0; i < 30; ++i) { m.ResetContext(); m.Observe("a"); m.Observe("c"); }

	m.ResetContext();
	m.Observe("a");
	// After decay, the recent continuation should win.
	CHECK(m.Predict() == "c");
}

TEST_CASE("json round-trip preserves learned state") {
	Config cfg; cfg.order = 2;
	MarkovModel m(cfg);
	for (int rep = 0; rep < 15; ++rep) FeedLoop(m, {"a", "b", "c", "d"});

	markov::json j = m.ToJson();
	MarkovModel loaded;
	REQUIRE(loaded.FromJson(j));

	CHECK(loaded.observations() == m.observations());
	CHECK(loaded.contexts() == m.contexts());

	// The reloaded model should predict identically from the same context.
	m.ResetContext();      m.Observe("a");      m.Observe("b");
	loaded.ResetContext(); loaded.Observe("a"); loaded.Observe("b");
	CHECK(loaded.Predict() == m.Predict());
}

TEST_CASE("save/load round-trips through a file") {
	const std::string path = "/tmp/markov_unit_state.json";
	Config cfg; cfg.order = 2;
	MarkovModel m(cfg);
	for (int rep = 0; rep < 15; ++rep) FeedLoop(m, {"e", "f", "g"});

	REQUIRE(m.Save(path));
	MarkovModel loaded;
	REQUIRE(loaded.Load(path));
	CHECK(loaded.observations() == m.observations());
	CHECK(loaded.contexts() == m.contexts());

	std::remove(path.c_str());
}

TEST_CASE("loading a missing file fails gracefully") {
	MarkovModel m;
	CHECK(m.Load("/tmp/does-not-exist-markov-xyz.json") == false);
	// Model remains usable (empty) after a failed load.
	CHECK(m.observations() == 0);
	double s = m.Observe("first-ever");
	CHECK(s == doctest::Approx(0.0));  // first event on empty model: not surprising
}
