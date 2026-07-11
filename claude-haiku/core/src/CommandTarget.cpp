#include "cch/CommandTarget.h"

#include <sstream>
#include <string>
#include <vector>

namespace cch {

// ── LocalTarget ──────────────────────────────────────────────────────────────

int LocalTarget::Run(const std::vector<std::string>& command,
                     const StreamSink& sink)
{
	Process p(command);
	return p.Run(sink);
}

// ── SshPosixTarget ───────────────────────────────────────────────────────────

SshPosixTarget::SshPosixTarget(std::string host, std::string controlSocket)
	: fHost(std::move(host))
	, fControlSocket(std::move(controlSocket))
{
}

// Build the ssh argv: ssh -S <socket> -o BatchMode=yes <host> <command...>
// We fork/exec ssh directly with an argv array (no local /bin/sh), so only
// the remote shell ever parses anything — single layer of quoting.
int SshPosixTarget::Run(const std::vector<std::string>& command,
                        const StreamSink& sink)
{
	std::vector<std::string> argv;
	argv.push_back("ssh");
	argv.push_back("-S");
	argv.push_back(fControlSocket);
	argv.push_back("-o");
	argv.push_back("BatchMode=yes");
	argv.push_back(fHost);
	// Append each remote command token as a separate argument.
	// ssh forwards them as a single remote command string with the
	// tokens space-joined and shell-interpreted on the remote side.
	for (const auto& tok : command) argv.push_back(tok);

	Process p(argv);
	return p.Run(sink);
}

bool SshPosixTarget::OpenMaster()
{
	// Open the ControlMaster connection so subsequent Run() calls share
	// the multiplexed channel (no per-command TLS handshake).
	// ssh -M -S <socket> -o BatchMode=yes -f -N <host>
	//   -f  : go to background after auth
	//   -N  : don't execute a remote command — just hold the channel open
	const std::vector<std::string> argv = {
		"ssh", "-M",
		"-S", fControlSocket,
		"-o", "BatchMode=yes",
		"-o", "ConnectTimeout=10",
		"-f", "-N",
		fHost,
	};
	// We only care about success/failure — the "output" is irrelevant.
	std::string dummy;
	StreamSink sink;
	sink.onChunk  = [&dummy](const std::string& c){ dummy += c; };
	sink.onDone   = [](int){};
	sink.onError  = [](const std::string&){};
	Process p(argv);
	return p.Run(sink) == 0;
}

void SshPosixTarget::CloseMaster()
{
	// Tell the master process to exit: ssh -S <socket> -O exit <host>
	const std::vector<std::string> argv = {
		"ssh", "-S", fControlSocket, "-O", "exit", fHost,
	};
	StreamSink sink;
	sink.onChunk  = [](const std::string&){};
	sink.onDone   = [](int){};
	sink.onError  = [](const std::string&){};
	Process p(argv);
	p.Run(sink); // ignore return — best-effort teardown
}

// ── SshRouterOsTarget ────────────────────────────────────────────────────────

SshRouterOsTarget::SshRouterOsTarget(std::string host, std::string controlSocket)
	: fHost(std::move(host))
	, fControlSocket(std::move(controlSocket))
{
}

// RouterOS uses the same SSH transport but a different "shell" on the remote
// side (RouterOS CLI, not bash). Commands are RouterOS syntax, e.g.
//   /interface bridge port print
// Output is RouterOS's terse columnar format. We still fork/exec ssh with
// the same ControlMaster socket so transport is identical to SshPosixTarget.
int SshRouterOsTarget::Run(const std::vector<std::string>& command,
                           const StreamSink& sink)
{
	std::vector<std::string> argv;
	argv.push_back("ssh");
	argv.push_back("-S");
	argv.push_back(fControlSocket);
	argv.push_back("-o");
	argv.push_back("BatchMode=yes");
	argv.push_back(fHost);
	for (const auto& tok : command) argv.push_back(tok);

	Process p(argv);
	return p.Run(sink);
}

} // namespace cch
