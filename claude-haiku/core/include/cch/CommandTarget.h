#ifndef CCH_COMMAND_TARGET_H
#define CCH_COMMAND_TARGET_H

#include <memory>
#include <string>
#include <vector>

#include "cch/Process.h"
#include "cch/StreamSink.h"

namespace cch {

// Where a command runs. Each implementation knows how to (a) turn a logical
// command into a concrete argv and (b) parse the host's output style. They all
// hand off to the one Process primitive, and they all stream through the one
// StreamSink. Transport and quoting differ; streaming and display do not.
class CommandTarget {
public:
    virtual ~CommandTarget() = default;

    // Run a logical command. For LocalTarget `command` is an argv already.
    // For SSH targets `command` is the remote command; the target wraps it.
    virtual int Run(const std::vector<std::string>& command,
                    const StreamSink& sink) = 0;

    // Human label for logs / UI ("local", "ssh:jetson", "routeros:crs520").
    virtual std::string Name() const = 0;
};

// --- Local --------------------------------------------------------------
// argv runs on this machine. On Haiku, may special-case file-search commands
// to a BFS BQuery instead of spawning find (injected by the GUI front-end;
// see FileSearch interface). Plain fork/exec otherwise.
class LocalTarget : public CommandTarget {
public:
    int Run(const std::vector<std::string>& command,
            const StreamSink& sink) override;
    std::string Name() const override { return "local"; }
};

// --- SSH to a POSIX host -------------------------------------------------
// Wraps argv in: ssh -S <controlsocket> -o BatchMode=yes <host> <argv...>
// ControlMaster multiplexed so there is no per-command handshake. Because we
// fork/exec ssh directly with an argv array (no local shell), only the REMOTE
// shell parses anything -- single layer of quoting.
class SshPosixTarget : public CommandTarget {
public:
    SshPosixTarget(std::string host, std::string controlSocket);
    int Run(const std::vector<std::string>& command,
            const StreamSink& sink) override;
    std::string Name() const override { return "ssh:" + fHost; }

    // Open/close the multiplexing master connection (once per session).
    bool OpenMaster();
    void CloseMaster();

private:
    std::string fHost;
    std::string fControlSocket;
};

// --- SSH to RouterOS (MikroTik) -----------------------------------------
// Same SSH transport, but the remote "shell" is RouterOS, not bash. Commands
// are RouterOS syntax (e.g. /interface bridge port print) and output parsing
// is its terse columnar format, not POSIX. Keeps your CRS520/CRS312 work in
// the same pipeline without pretending it is a POSIX shell.
class SshRouterOsTarget : public CommandTarget {
public:
    SshRouterOsTarget(std::string host, std::string controlSocket);
    int Run(const std::vector<std::string>& command,
            const StreamSink& sink) override;
    std::string Name() const override { return "routeros:" + fHost; }

private:
    std::string fHost;
    std::string fControlSocket;
};

} // namespace cch

#endif
