#ifndef CCH_AGENT_LOOP_H
#define CCH_AGENT_LOOP_H

#include <string>
#include <vector>

#include "cch/ApiClient.h"
#include "cch/StreamSink.h"
#include "cch/Tools.h"

namespace cch {

// The orchestrator and the only stateful conversation owner. One Turn() does:
//
//   1. Append the user message to history.
//   2. Stream an assistant turn via ApiClient (text -> sink.onChunk).
//   3. If the model requested tools: dispatch each through the registry
//      (live output also -> sink), collect tool_results.
//   4. Append assistant + tool_result messages, loop back to 2.
//   5. Stop when the model returns a turn with no tool_use. sink.onDone.
//
// Runs on a worker thread. The injected sink marshals everything to the UI.
class AgentLoop {
public:
    AgentLoop(ApiClient& api, ToolRegistry& tools, std::string systemPrompt);

    // Drive one full user turn to completion (may span several API round-trips
    // if tools are involved). Streams via sink throughout.
    void Turn(const std::string& userText, const StreamSink& sink);

    // History is retained between Turn() calls for multi-turn context.
    void Reset();
    const std::vector<Message>& History() const { return fHistory; }

private:
    ApiClient&            fApi;
    ToolRegistry&         fTools;
    std::string           fSystemPrompt;
    std::vector<Message>  fHistory;
};

} // namespace cch

#endif
