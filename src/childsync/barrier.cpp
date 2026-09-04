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

		long futex(uint32_t* word, int op, uint32_t value, const struct timespec* timeout,
				   uint32_t bitset = 0) noexcept {
			return ::syscall(SYS_futex, word, op, value, timeout, nullptr, bitset);
		}

	} // namespace

	// Not the _PRIVATE variants: those key the futex by process address space, so the parent and a
	// child sharing the mapping would compute different keys and never see each other's waits.
	int futex_wake(uint32_t& word, int count) noexcept {
		return static_cast<int>(futex(&word, FUTEX_WAKE, static_cast<uint32_t>(count), nullptr));
	}

	int futex_wake_bitset(uint32_t& word, int count, uint32_t mask) noexcept {
		return static_cast<int>(futex(&word, FUTEX_WAKE_BITSET, static_cast<uint32_t>(count), nullptr, mask));
	}

	bool futex_wait_bitset(uint32_t& word, uint32_t expected, uint32_t mask,
						   std::chrono::microseconds timeout) noexcept {
		struct timespec now{};
		(void)::clock_gettime(CLOCK_MONOTONIC, &now);
		const int64_t   nanos = static_cast<int64_t>(now.tv_nsec) + timeout.count() * 1000;
		struct timespec deadline{};
		deadline.tv_sec   = now.tv_sec + nanos / 1000000000;
		deadline.tv_nsec  = nanos % 1000000000;
		const long result = futex(&word, FUTEX_WAIT_BITSET, expected, &deadline, mask);
		return !(result < 0 && errno == ETIMEDOUT);
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
