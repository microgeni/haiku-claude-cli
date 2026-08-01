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
