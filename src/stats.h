#ifndef HAIKU_CLAUDE_CLI_STATS_H
#define HAIKU_CLAUDE_CLI_STATS_H

#include <string>

// Persistent lifetime stats for the CLI. Counters are written to
// <config_dir>/stats.json after every recorded event so they
// survive crashes and session restarts. Call sites are:
//
//   - stats::record_session() once at REPL startup
//   - stats::record_turn()    after each assistant turn completes
//   - stats::record_tool()    after each tool invocation (BFS tools
//                             also pass `saved_bytes` so /stats can
//                             show the Haiku-advantage savings)
//   - stats::format_display() renders the /stats slash-command output
//
// The saved_bytes channel is what powers the "BFS saved N tokens"
// block in /stats — it's the bytes a full Read would have cost
// minus the bytes ReadAttr/Query actually returned.
namespace stats {

void record_session();
void record_turn(int input_tokens, int output_tokens);
void record_tool(const std::string& tool_name, int result_bytes,
                 long saved_bytes = 0);

std::string format_display();

} // namespace stats

#endif
