#ifndef CCH_API_CLIENT_H
#define CCH_API_CLIENT_H

#include <string>
#include <vector>

#include "cch/StreamSink.h"
#include "cch/Tools.h"

namespace cch {

// One message in the conversation as sent to /v1/messages.
struct Message {
    std::string role;     // "user" | "assistant"
    std::string content;  // text; tool_use / tool_result handled by the loop
};

// Owns HTTP/TLS (curl or mbedTLS from HaikuPorts -- the single riskiest Haiku
// dependency; prototype this against the real endpoint FIRST). Builds the
// request, streams the SSE response: text deltas drive sink.onChunk, and any
// tool_use blocks are collected into `toolCalls` for the agent loop.
//
// Knows nothing about where tokens are displayed. Pure transport + parse.
class ApiClient {
public:
    struct Config {
        std::string model = "claude-opus-4-8";
        std::string baseUrl = "https://api.anthropic.com/v1/messages";
        int         maxTokens = 4096;
        // API key is read from the environment by the transport layer; never
        // stored or passed through the GUI.
    };

    explicit ApiClient(Config cfg);
    ~ApiClient();

    // Send one turn. Streams assistant text via sink.onChunk. On completion,
    // any tool_use blocks the model emitted are appended to `outToolCalls` and
    // sink.onDone is called. Network/parse failure -> sink.onError.
    void SendStreaming(const std::vector<Message>& history,
                       const std::string& systemPrompt,
                       const std::string& toolSchemaJson,
                       const StreamSink& sink,
                       std::vector<ToolCall>& outToolCalls);

private:
    Config fCfg;
};

} // namespace cch

#endif
