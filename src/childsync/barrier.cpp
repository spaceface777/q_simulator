#include "barrier.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace childsync {

	namespace {

		long futex(uint32_t* word, int op, uint32_t value, const struct timespec* timeout) noexcept {
			return ::syscall(SYS_futex, word, op, value, timeout, nullptr, 0);
		}

	} // namespace

	// Not the _PRIVATE variants: those key the futex by process address space, so the parent and a
	// child sharing the mapping would compute different keys and never see each other's waits.
	int futex_wake(uint32_t& word, int count) noexcept {
		return static_cast<int>(futex(&word, FUTEX_WAKE, static_cast<uint32_t>(count), nullptr));
	}

	bool futex_wait(uint32_t& word, uint32_t expected, std::chrono::microseconds timeout) noexcept {
		struct timespec spec{};
		spec.tv_sec  = timeout.count() / 1000000;
		spec.tv_nsec = (timeout.count() % 1000000) * 1000;

		const long result = futex(&word, FUTEX_WAIT, expected, &spec);
		return !(result < 0 && errno == ETIMEDOUT);
	}

	void Mapping::reset() noexcept {
		if (base_ != nullptr) {
			::munmap(base_, bytes_);
			base_ = nullptr;
		}
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
		bytes_ = 0;
	}

	Mapping::Mapping(const char* name, std::size_t bytes) : bytes_(bytes) {
		fd_ = static_cast<int>(::syscall(SYS_memfd_create, name, 0u));
		if (fd_ < 0) {
			throw std::runtime_error("Failed to create shared segment: " + std::string(::strerror(errno)));
		}

		if (::ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
			const std::string reason = ::strerror(errno);
			reset();
			throw std::runtime_error("Failed to size shared segment: " + reason);
		}

		base_ = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
		if (base_ == MAP_FAILED) {
			const std::string reason = ::strerror(errno);
			base_                    = nullptr;
			reset();
			throw std::runtime_error("Failed to map shared segment: " + reason);
		}

		// memfd pages read as zero, so every counter and offset starts cleared.
	}

	Mapping::Mapping(int fd, std::size_t bytes) : fd_(fd), bytes_(bytes) {
		base_ = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (base_ == MAP_FAILED) {
			const std::string reason = ::strerror(errno);
			base_                    = nullptr;
			fd_                      = -1;
			throw std::runtime_error("Failed to map inherited segment: " + reason);
		}
	}

	Mapping::~Mapping() {
		reset();
	}

	Mapping::Mapping(Mapping&& other) noexcept : fd_(other.fd_), base_(other.base_), bytes_(other.bytes_) {
		other.fd_    = -1;
		other.base_  = nullptr;
		other.bytes_ = 0;
	}

	Mapping& Mapping::operator=(Mapping&& other) noexcept {
		if (this != &other) {
			reset();
			fd_          = other.fd_;
			base_        = other.base_;
			bytes_       = other.bytes_;
			other.fd_    = -1;
			other.base_  = nullptr;
			other.bytes_ = 0;
		}
		return *this;
	}

	void Mapping::keep_across_exec() const {
		const int flags = ::fcntl(fd_, F_GETFD);
		if (flags < 0 || ::fcntl(fd_, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
			throw std::runtime_error("Failed to clear FD_CLOEXEC on shared segment: " + std::string(::strerror(errno)));
		}
	}

} // namespace childsync
