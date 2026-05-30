#include "cch/Process.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace cch {

Process::Process(std::vector<std::string> argv)
	: fArgv(std::move(argv))
{
}

Process::~Process()
{
	// If Run() was never called or completed, nothing to do.
	// If the child is somehow still alive, cancel it.
	if (fChildPid > 0) {
		Cancel();
		int status = 0;
		::waitpid(static_cast<pid_t>(fChildPid), &status, WNOHANG);
		fChildPid = -1;
	}
}

int Process::Run(const StreamSink& sink)
{
	if (fArgv.empty()) {
		if (sink.onError) sink.onError("Process::Run: empty argv");
		return -1;
	}

	// Build a null-terminated argv array for execvp.
	std::vector<const char*> argv;
	argv.reserve(fArgv.size() + 1);
	for (const auto& s : fArgv) argv.push_back(s.c_str());
	argv.push_back(nullptr);

	// stdout+stderr pipe. A single pipe keeps the output stream in
	// order. If the sink has a separate onStderr, we'd need a second
	// pipe — leave that as a future refinement.
	int pipefd[2];
	if (::pipe(pipefd) != 0) {
		if (sink.onError)
			sink.onError(std::string("pipe() failed: ") + std::strerror(errno));
		return -1;
	}

	const pid_t pid = ::fork();
	if (pid < 0) {
		::close(pipefd[0]);
		::close(pipefd[1]);
		if (sink.onError)
			sink.onError(std::string("fork() failed: ") + std::strerror(errno));
		return -1;
	}

	if (pid == 0) {
		// Child: run in its own process group so kill(-pgid, ...) reaches
		// any grandchildren too.
		::setsid();

		// Redirect stdin to /dev/null so the child cannot read from or
		// corrupt the parent's terminal.
		const int devnull = ::open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			::dup2(devnull, STDIN_FILENO);
			if (devnull > STDERR_FILENO) ::close(devnull);
		}

		::close(pipefd[0]);
		if (::dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
		if (::dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
		::close(pipefd[1]);

		::execvp(argv[0], const_cast<char* const*>(argv.data()));
		_exit(127);
	}

	// Parent.
	fChildPid = static_cast<long>(pid);
	::close(pipefd[1]);

	const auto start_time = std::chrono::steady_clock::now();

	char buf[4096];
	bool killed = false;

	while (true) {
		if (fCancel.load(std::memory_order_relaxed)) {
			killed = true;
			break;
		}

		if (fTimeoutMs > 0) {
			const double elapsed = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start_time).count();
			if (elapsed >= static_cast<double>(fTimeoutMs)) {
				killed = true;
				break;
			}
		}

		struct pollfd pfd {};
		pfd.fd     = pipefd[0];
		pfd.events = POLLIN;
		const int r = ::poll(&pfd, 1, 100); // 100 ms tick
		if (r < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (r == 0) continue; // tick — re-check flags

		if (!(pfd.revents & POLLIN)) break; // HUP/ERR — pipe closed

		const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
		if (n <= 0) break; // EOF
		if (sink.onChunk)
			sink.onChunk(std::string(buf, static_cast<size_t>(n)));
	}

	::close(pipefd[0]);

	if (killed) {
		// SIGTERM the entire process group, then SIGKILL after 200 ms.
		::kill(-pid, SIGTERM);
		struct timespec ts { 0, 200'000'000 };
		::nanosleep(&ts, nullptr);
		::kill(-pid, SIGKILL);
	}

	int status = 0;
	::waitpid(pid, &status, 0);
	fChildPid = -1;

	if (killed) {
		if (sink.onError) sink.onError("[cancelled]");
		return -1;
	}

	if (!WIFEXITED(status)) {
		if (sink.onError) sink.onError("process terminated abnormally");
		return -1;
	}

	const int code = WEXITSTATUS(status);
	if (sink.onDone) sink.onDone(code);
	return code;
}

void Process::Cancel()
{
	fCancel.store(true, std::memory_order_relaxed);
	// If the child is already running, poke it immediately rather than
	// waiting for the next poll tick.
	if (fChildPid > 0) {
		::kill(-static_cast<pid_t>(fChildPid), SIGTERM);
	}
}

} // namespace cch
