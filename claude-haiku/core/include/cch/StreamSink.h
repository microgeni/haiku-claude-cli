#ifndef CCH_STREAM_SINK_H
#define CCH_STREAM_SINK_H

#include <functional>
#include <string>

namespace cch {

// The single streaming contract. Anything that produces text incrementally
// -- an API response OR a running command -- drives a StreamSink. The core is
// front-end-agnostic because it only ever sees this. The GUI implements the
// callbacks as "post a BMessage to the window"; the CLI implements them as
// "write to the TUI render loop".
//
// Thread note: these callbacks are invoked from a worker thread. Implementations
// MUST NOT touch BViews directly -- marshal across to the main thread.
struct StreamSink {
    // A chunk of text arrived (an SSE token, or a line of command stdout).
    std::function<void(const std::string& chunk)> onChunk;

    // Stream finished cleanly. `code` is the model stop reason category for an
    // API stream, or the process exit code for a command.
    std::function<void(int code)> onDone;

    // Stream aborted. `message` is human-readable; safe to display.
    std::function<void(const std::string& message)> onError;

    // Optional out-of-band: a chunk specifically from stderr (command targets).
    // If unset, implementations fold stderr into onChunk.
    std::function<void(const std::string& chunk)> onStderr;
};

} // namespace cch

#endif
