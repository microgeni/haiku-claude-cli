#ifndef CCH_PROCESS_H
#define CCH_PROCESS_H

#include <atomic>
#include <string>
#include <vector>

#include "cch/StreamSink.h"

namespace cch {

// The foundation primitive every CommandTarget sits on. Runs argv via
// fork/exec (NOT a shell -- no /bin/sh, so quoting happens at most once, on the
// remote side only for SSH). Captures stdout/stderr through pipes and streams
// them into the sink. Holds the child PID so the command is killable on
// timeout or user cancel.
//
// Runs synchronously on the calling (worker) thread: it blocks reading pipes
// until the child exits or Cancel() flips the flag. Spawn the worker around it.
class Process {
public:
    // argv[0] is the program; the vector is passed straight to execvp.
    Process(std::vector<std::string> argv);
    ~Process();

    // Blocks until completion. Drives sink.onChunk / onStderr as output
    // arrives, then sink.onDone(exitCode) or sink.onError(...).
    // Returns the process exit code, or -1 on spawn/exec failure.
    int Run(const StreamSink& sink);

    // Thread-safe: ask the running child to stop. The Run loop polls this
    // between reads and SIGTERMs (then SIGKILLs) the child PID.
    void Cancel();

    // Optional wall-clock limit; 0 = no timeout. Enforced in the read loop.
    void SetTimeoutMs(int ms) { fTimeoutMs = ms; }

private:
    std::vector<std::string> fArgv;
    std::atomic<bool>        fCancel{false};
    int                      fTimeoutMs{0};
    long                     fChildPid{-1};
};

} // namespace cch

#endif
