#ifndef CHILDPROC_LAUNCHER_H
#define CHILDPROC_LAUNCHER_H

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

#include "../module/types.h"

namespace childproc {

	// Minimal on purpose: give the child only what real hardware would know at boot.
	struct LaunchSpec {
		module::ModuleId id;
		std::string      binary;
		int              control_fd = -1; // The shared barrier words.
		int              mailbox_fd = -1; // This child's two mailboxes, and nobody else's.
		uint64_t         seed       = 0;
	};

	class Handle {
	private:
		pid_t pid_    = -1;
		bool  reaped_ = false;
		int   status_ = 0;

	public:
		Handle() = default;

		explicit Handle(pid_t pid) : pid_(pid) {}

		~Handle();

		Handle(const Handle&)            = delete;
		Handle& operator=(const Handle&) = delete;
		Handle(Handle&& other) noexcept;
		Handle& operator=(Handle&& other) noexcept;

		[[nodiscard]] pid_t pid() const noexcept { return pid_; }

		// Non-blocking: returns nothing until the child has exited.
		[[nodiscard]] std::optional<int> exit_status();

		void terminate() noexcept;
	};

	// --no-rt makes the child's clock advance only on a granted tick, not on wall time.
	[[nodiscard]] Handle launch(const LaunchSpec& spec);

} // namespace childproc

#endif // CHILDPROC_LAUNCHER_H
