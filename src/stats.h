#ifndef HAIKU_CLAUDE_CLI_STATS_H
#define HAIKU_CLAUDE_CLI_STATS_H

#include <string>

// Persistent lifetime stats for the CLI. Counters are written to
// <ConfigDir>/stats.json after every recorded event so they
// survive crashes and session restarts. Call sites are:
//
//   - stats::RecordSession() once at REPL startup
//   - stats::RecordTurn()    after each assistant turn completes
//   - stats::RecordTool()    after each tool invocation (BFS tools
//                             also pass `savedBytes` so /stats can
//                             show the Haiku-advantage savings)
//   - stats::FormatDisplay() renders the /stats slash-command output
//
// The savedBytes channel is what powers the "BFS saved N tokens"
// block in /stats — it's the bytes a full Read would have cost
// minus the bytes ReadAttr/Query actually returned.
namespace stats {

void RecordSession();
void RecordTurn(int input_tokens, int output_tokens,
				int cache_read_tokens = 0, int cache_write_tokens = 0);
void RecordTool(const std::string& tool_name, int result_bytes,
				 long savedBytes = 0);

std::string FormatDisplay();

} // namespace stats

#endif
