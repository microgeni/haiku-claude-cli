// Unit tests for workflow::MakeKey — the canonical event-key builder that
// turns a (tool_name, tool_input, is_error) tuple into the stable string the
// Markov workflow-memory learns over. The exact key format is an internal
// detail, so these assert the *properties* the design relies on rather than
// pinning exact strings:
//
//   * edits to the same file/module produce the same (or overlapping) key,
//   * success and failure of the same tool are distinguished,
//   * bash commands key on the program name, not the full command line,
//   * reads are path-keyed and status-independent,
//   * unknown/MCP tools still produce a coherent key.
//
// Build: see the `test-unit` target in the top-level Makefile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/workflow.h"

#include <cstdlib>
#include <string>

using workflow::MakeKey;
using nlohmann::json;

TEST_CASE("edit keys are path-scoped and status-tagged") {
	json in = {{"path", "src/auth.py"}};
	std::string ok  = MakeKey("Edit", in, /*is_error=*/false);
	std::string err = MakeKey("Edit", in, /*is_error=*/true);

	CHECK(ok.rfind("edit:", 0) == 0);
	CHECK(ok != err);                       // success vs failure differ
	CHECK(ok.find("auth.py") != std::string::npos);
}

TEST_CASE("Write and Edit share the edit namespace") {
	json in = {{"path", "src/auth.py"}};
	// Both are file-mutating ops; keying both as "edit:" lets them reinforce.
	CHECK(MakeKey("Write", in, false).rfind("edit:", 0) == 0);
	CHECK(MakeKey("Edit",  in, false).rfind("edit:", 0) == 0);
}

TEST_CASE("same module, same key regardless of absolute prefix depth") {
	// Only the last couple of path components are kept, so deep vs shallow
	// references to the same file collapse together.
	std::string a = MakeKey("Edit", {{"path", "/home/me/proj/src/auth.py"}}, false);
	std::string b = MakeKey("Edit", {{"path", "src/auth.py"}}, false);
	CHECK(a == b);
}

TEST_CASE("reads are path-keyed and status-independent") {
	json in = {{"path", "src/markov.h"}};
	std::string ok  = MakeKey("Read", in, false);
	std::string err = MakeKey("Read", in, true);
	CHECK(ok.rfind("read:", 0) == 0);
	CHECK(ok == err);                       // read status is not distinguished
	CHECK(ok.find("markov.h") != std::string::npos);
}

TEST_CASE("bash keys on the program name, not the full command") {
	std::string a = MakeKey("Bash", {{"command", "git commit -m 'x'"}}, false);
	std::string b = MakeKey("Bash", {{"command", "git status"}}, false);
	// Different git invocations collapse to the same program key.
	CHECK(a == b);
	CHECK(a.rfind("run:", 0) == 0);
	CHECK(a.find("git") != std::string::npos);
}

TEST_CASE("bash success and failure differ") {
	json in = {{"command", "make"}};
	CHECK(MakeKey("Bash", in, false) != MakeKey("Bash", in, true));
}

TEST_CASE("bash program basename is normalized") {
	// "./run.sh" and "/usr/local/bin/run.sh" should not fragment.
	std::string a = MakeKey("Bash", {{"command", "./run.sh --fast"}}, false);
	std::string b = MakeKey("Bash", {{"command", "/usr/local/bin/run.sh"}}, false);
	CHECK(a == b);
}

TEST_CASE("search tools collapse by kind") {
	CHECK(MakeKey("Grep", {{"pattern", "foo"}}, false) ==
	      MakeKey("Grep", {{"pattern", "bar"}}, false));
	CHECK(MakeKey("Glob", {{"pattern", "*.cpp"}}, false).rfind("search:", 0) == 0);
}

TEST_CASE("unknown / MCP tools still produce a coherent key") {
	std::string k = MakeKey("mcp__server__do_thing", json::object(), false);
	CHECK(!k.empty());
	// Falls back to lowercased tool name + status.
	CHECK(k.find("mcp__server__do_thing") != std::string::npos);
}

TEST_CASE("missing fields do not crash") {
	// Robust against inputs lacking the expected key.
	CHECK(!MakeKey("Edit", json::object(), false).empty());
	CHECK(!MakeKey("Bash", json::object(), true).empty());
	CHECK(!MakeKey("Read", json::object(), false).empty());
}

// ---------------------------------------------------------------------------
// Observe() — the nudge itself. These drive the real model, so they point
// HOME at a temp directory to keep the per-repo state file out of the
// developer's real config dir.
// ---------------------------------------------------------------------------

namespace {

// Bind a fresh, enabled model to an isolated state directory.
void BeginIsolated(const std::string& tag) {
	const std::string home = "/tmp/wf_unit_" + tag;
	setenv("HOME", home.c_str(), 1);
	workflow::Configure(json{{"enabled", true}, {"nudges", true}});
	workflow::Begin("/fake/repo/" + tag);
}

// Teach the model the habit read -> edit -> make, `times` times over.
void LearnHabit(int times) {
	for (int i = 0; i < times; ++i) {
		workflow::Observe("Read", json{{"path", "src/api.cpp"}}, false);
		workflow::Observe("Edit", json{{"path", "src/api.cpp"}}, false);
		workflow::Observe("Bash", json{{"command", "make"}}, false);
	}
}

} // namespace

TEST_CASE("a learned habit stops being surprising") {
	BeginIsolated("habit");
	LearnHabit(8);
	workflow::Observe("Read", json{{"path", "src/api.cpp"}}, false);
	workflow::Observe("Edit", json{{"path", "src/api.cpp"}}, false);
	CHECK(workflow::Observe("Bash", json{{"command", "make"}}, false).empty());
}

// Regression: Predict() must be sampled BEFORE Observe() advances the
// context. Sampling it after answers "what follows the anomaly?", which is
// empty for a novel context — so the useful half of the nudge never showed.
TEST_CASE("a nudge names the step that was expected instead") {
	BeginIsolated("expected");
	LearnHabit(8);
	workflow::Observe("Read", json{{"path", "src/api.cpp"}}, false);
	workflow::Observe("Edit", json{{"path", "src/api.cpp"}}, false);
	const std::string nudge =
		workflow::Observe("Bash", json{{"command", "git push"}}, false);

	REQUIRE_FALSE(nudge.empty());
	CHECK(nudge.find("run:git:ok") != std::string::npos);   // what happened
	CHECK(nudge.find("run:make:ok") != std::string::npos);  // what was expected
	CHECK(nudge.find("you usually do") != std::string::npos);
}

TEST_CASE("a failed build where a clean one was expected is flagged") {
	BeginIsolated("failing");
	LearnHabit(8);
	workflow::Observe("Read", json{{"path", "src/api.cpp"}}, false);
	workflow::Observe("Edit", json{{"path", "src/api.cpp"}}, false);
	const std::string nudge =
		workflow::Observe("Bash", json{{"command", "make"}}, /*is_error=*/true);

	REQUIRE_FALSE(nudge.empty());
	CHECK(nudge.find("run:make:err") != std::string::npos);
}

TEST_CASE("nudges:false learns silently") {
	setenv("HOME", "/tmp/wf_unit_silent", 1);
	workflow::Configure(json{{"enabled", true}, {"nudges", false}});
	workflow::Begin("/fake/repo/silent");
	LearnHabit(8);
	workflow::Observe("Read", json{{"path", "src/api.cpp"}}, false);
	workflow::Observe("Edit", json{{"path", "src/api.cpp"}}, false);
	CHECK(workflow::Observe("Bash", json{{"command", "git push"}}, false).empty());
}
