#include "launcher.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace childproc {

	namespace {

		constexpr int EXEC_FAILED = 127; // Used when the child fails between fork() and exec().

	} // namespace

	Handle::~Handle() {
		terminate();
	}

	Handle::Handle(Handle&& other) noexcept : pid_(other.pid_), reaped_(other.reaped_), status_(other.status_) {
		other.pid_    = -1;
		other.reaped_ = true;
	}

	Handle& Handle::operator=(Handle&& other) noexcept {
		if (this != &other) {
			terminate();
			pid_          = other.pid_;
			reaped_       = other.reaped_;
			status_       = other.status_;
			other.pid_    = -1;
			other.reaped_ = true;
		}
		return *this;
	}

	std::optional<int> Handle::exit_status() {
		if (pid_ < 0) {
			return {};
		}
		if (reaped_) {
			return status_;
		}

		int         status = 0;
		const pid_t result = ::waitpid(pid_, &status, WNOHANG);
		if (result != pid_) {
			return {};
		}

		reaped_ = true;
		status_ = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
		return status_;
	}

	void Handle::terminate() noexcept {
		if (pid_ < 0 || reaped_) {
			pid_ = -1;
			return;
		}

		::kill(pid_, SIGKILL);

		int status = 0;
		while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
		}

		reaped_ = true;
		pid_    = -1;
	}

	Handle launch(const LaunchSpec& spec) {
		// Do not accumulate forked copies of the simulator's compiled specification.
		// EOF acknowledges exec (or failure), not application activation. CLOEXEC also
		// prevents later children from inheriting an acknowledgement pipe.
		int ready[2];
		if (::pipe2(ready, O_CLOEXEC) < 0)
			throw std::runtime_error("Failed to create module launch pipe: " + std::string(::strerror(errno)));
		const pid_t pid = ::fork();
		if (pid < 0) {
			const int error = errno;
			::close(ready[0]);
			::close(ready[1]);
			throw std::runtime_error("Failed to fork module " + std::to_string(spec.id.raw()) + ": " +
									 std::string(::strerror(error)));
		}

		if (pid > 0) {
			::close(ready[1]);
			char unused;
			while (::read(ready[0], &unused, 1) < 0 && errno == EINTR) {
			}
			::close(ready[0]);
			return Handle(pid);
		}
		::close(ready[0]);

		// Child process: must not throw or allocate, so failures call _exit(), not an exception.
		::prctl(PR_SET_PDEATHSIG, SIGKILL);

		::setenv("QSIM_CTRL_FD", std::to_string(spec.control_fd).c_str(), 1);
		::setenv("QSIM_MBOX_FD", std::to_string(spec.mailbox_fd).c_str(), 1);
		::setenv("QSIM_MODULE_ID", std::to_string(spec.id.raw()).c_str(), 1);

		const std::string name = "quercus-module-" + std::to_string(spec.id.raw());
		const std::string seed = "--seed=" + std::to_string(spec.seed);

		::execl(spec.binary.c_str(), name.c_str(), "--no-rt", seed.c_str(), static_cast<char*>(nullptr));

		::_exit(EXEC_FAILED); // Reached only if exec() failed.
	}

} // namespace childproc
