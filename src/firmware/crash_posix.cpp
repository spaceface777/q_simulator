#define _COSMO_SOURCE
#include "crash.h"
#include "crash_walk.h"
#include "crash_watchdog.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <setjmp.h>
#include <system_error>
#include <ucontext.h>
#include <unistd.h>

#ifdef __COSMOPOLITAN__
#include <libc/dce.h>
#ifdef __x86_64__
#include <libc/nt/enum/exceptionhandleractions.h>
#include <libc/nt/enum/memflags.h>
#include <libc/nt/enum/pageflags.h>
#include <libc/nt/enum/status.h>
#include <libc/nt/memory.h>
#include <libc/nt/signals.h>
#include <libc/nt/struct/ntexceptionrecord.h>
#endif
#endif

namespace firmware::crash {
	namespace {
		constexpr int WATCHDOG_SIGNAL = SIGUSR2;

		struct Guard {
			sigjmp_buf            jump;
			Capture&              capture;
			uint64_t              call;
			volatile sig_atomic_t active;
		};

		thread_local Guard*   current = nullptr;
		thread_local sigset_t outside_mask;
		thread_local bool     prepared          = false;
		thread_local char*    host_random_state = nullptr;
		alignas(uint32_t) thread_local char scratch_random_state[128];
		alignas(16) thread_local char signal_stack[128 * 1024];
		// resume() needs little more than siglongjmp, but under AddressSanitizer the interceptor of
		// siglongjmp runs on it too, and reports there. 4 KiB overflowed into other thread locals.
		alignas(16) thread_local char resume_stack[64 * 1024];
		std::once_flag        installed;
		struct sigaction      previous[NSIG];
		int                   probe[2] = {-1, -1};
		std::mutex            interrupt_mutex;
		pthread_t             watched_thread;
		std::atomic<uint64_t> interrupted_call{0};

		uintptr_t strip(uintptr_t address) noexcept {
#if defined(__aarch64__)
			return address & ((uintptr_t{1} << 48) - 1);
#else
			return address;
#endif
		}

		bool read_memory(uintptr_t address, void* out, std::size_t size) noexcept {
			if (!address) return false;
#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
			// Windows pipe writes can fault on an invalid user buffer.
			if (IsWindows()) {
				NtMemoryBasicInformation region{};
				const auto*              source = reinterpret_cast<const char*>(address);
				if (!VirtualQuery(source, &region, sizeof(region))) return false;
				const auto         offset   = address - reinterpret_cast<uintptr_t>(region.BaseAddress);
				constexpr uint32_t readable = kNtPageReadonly | kNtPageReadwrite | kNtPageWritecopy |
											  kNtPageExecuteRead | kNtPageExecuteReadwrite | kNtPageExecuteWritecopy;
				if (region.State != kNtMemCommit || !(region.Protect & readable) || (region.Protect & kNtPageGuard) ||
					offset >= region.RegionSize || size > region.RegionSize - offset)
					return false;
				auto* bytes = static_cast<char*>(out);
				for (std::size_t i = 0; i < size; ++i) bytes[i] = source[i];
				return true;
			}
#endif
			if (probe[1] < 0) return false;
			const ssize_t written = ::write(probe[1], reinterpret_cast<const void*>(address), size);
			if (written <= 0) return false;
			char       discard[16];
			const bool complete = static_cast<std::size_t>(written) == size;
			return ::read(probe[0], complete ? out : discard, written) == written && complete;
		}

		void fill(Capture& capture, Cause cause, int signal, const siginfo_t* info,
				  const ucontext_t& context) noexcept {
			const auto& registers = context.uc_mcontext;
#if defined(__aarch64__)
			const uintptr_t pc = strip(registers.pc);
			const uintptr_t fp = registers.regs[29];
			const uintptr_t sp = registers.sp;
#else
			const auto* regs = registers.gregs;
			const auto  pc   = static_cast<uintptr_t>(regs[REG_RIP]);
			const auto  fp   = static_cast<uintptr_t>(regs[REG_RBP]);
			const auto  sp   = static_cast<uintptr_t>(regs[REG_RSP]);
#endif
			capture.cause    = cause;
			capture.signal   = signal;
			capture.limit_ms = cause == Cause::HANG ? watchdog_limit_ms() : 0;
			capture.address =
				info && (signal == SIGSEGV || signal == SIGBUS) ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
			capture.frames[0]   = pc;
			capture.frame_count = 1;
			walk_frames(capture, fp, sp, read_memory, strip);
		}

		[[noreturn]] void resume() {
			siglongjmp(current->jump, 1);
		}

#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
		// A Windows guard page becomes writable before the exception is delivered,
		// giving the exception dispatcher space on the exhausted coroutine stack.
		int32_t __attribute__((ms_abi)) stack_overflow(NtExceptionPointers* exception) {
			Guard*      guard  = current;
			const auto& record = *exception->ExceptionRecord;
			if (!guard || !guard->active || record.ExceptionCode != kNtStatusGuardPageViolation ||
				record.NumberParameters < 2)
				return kNtExceptionContinueSearch;

			const auto& registers     = *exception->ContextRecord;
			const auto  fault_address = record.ExceptionInformation[1];
			// A stack probe may touch the guard before moving the stack pointer.
			NtMemoryBasicInformation fault{}, stack{};
			if (!VirtualQuery(reinterpret_cast<const void*>(fault_address), &fault, sizeof(fault)) ||
				!VirtualQuery(reinterpret_cast<const void*>(registers.Rsp), &stack, sizeof(stack)) ||
				fault.AllocationBase != stack.AllocationBase)
				return kNtExceptionContinueSearch;

			guard->active       = 0;
			Capture& capture    = guard->capture;
			capture.cause       = Cause::STACK_OVERFLOW;
			capture.signal      = SIGSEGV;
			capture.address     = fault_address;
			capture.frames[0]   = registers.Rip;
			capture.frame_count = 1;
			walk_frames(capture, registers.Rbp, registers.Rsp, read_memory, strip);
			resume();
		}
#endif

		void handle(int signal, siginfo_t* info, void* raw) {
			const int  saved_errno = errno;
			const bool hang        = signal == WATCHDOG_SIGNAL;
			Guard*     guard       = current;
			if (guard && guard->active && (!hang || guard->call == interrupted_call.load(std::memory_order_acquire))) {
				guard->active = 0;
				auto& context = *static_cast<ucontext_t*>(raw);
				fill(guard->capture, hang ? Cause::HANG : Cause::FAULT, signal, info, context);
#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
				if (IsWindows()) resume();
#endif
				// Complete sigreturn so the OS releases the alternate stack before another fault.
				const auto top = reinterpret_cast<uintptr_t>(resume_stack + sizeof(resume_stack));
#if defined(__aarch64__)
				context.uc_mcontext.pc       = reinterpret_cast<uintptr_t>(&resume);
				context.uc_mcontext.sp       = top;
				context.uc_mcontext.regs[29] = 0;
				context.uc_mcontext.regs[30] = 0;
#else
				context.uc_mcontext.gregs[REG_RIP] = reinterpret_cast<uintptr_t>(&resume);
				context.uc_mcontext.gregs[REG_RSP] = top - 8;
				context.uc_mcontext.gregs[REG_RBP] = 0;
#endif
			} else if (!hang) {
				// A simulator fault keeps its original signal disposition.
				sigaction(signal, &previous[signal], nullptr);
				if (signal == SIGTRAP || !info || info->si_code <= 0) raise(signal);
			}
			errno = saved_errno;
		}

		void interrupt(uint64_t call) {
			std::lock_guard lock(interrupt_mutex);
			if (call != watchdog::watched()) return;
			interrupted_call.store(call, std::memory_order_release);
			pthread_kill(watched_thread, WATCHDOG_SIGNAL);
		}

		void install() {
			if (::pipe(probe) == 0) {
				for (const int fd : probe) {
					::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
					::fcntl(fd, F_SETFD, FD_CLOEXEC);
				}
			}

			struct sigaction action{};
			action.sa_sigaction = handle;
			action.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
			sigemptyset(&action.sa_mask);
			for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT}) {
				if (sigaction(signal, &action, &previous[signal]) != 0)
					throw std::system_error(errno, std::generic_category(), "install firmware fault handler");
			}
			action.sa_flags = SA_SIGINFO | SA_ONSTACK;
			if (sigaction(WATCHDOG_SIGNAL, &action, &previous[WATCHDOG_SIGNAL]) != 0)
				throw std::system_error(errno, std::generic_category(), "install firmware watchdog handler");
#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
			if (IsWindows() &&
				!AddVectoredExceptionHandler(1, reinterpret_cast<NtVectoredExceptionHandler>(&stack_overflow)))
				throw std::runtime_error("install firmware stack overflow handler");
#endif
			watchdog::start(interrupt);
		}

		void prepare_thread() {
			if (prepared) return;
			const int error = pthread_sigmask(SIG_SETMASK, nullptr, &outside_mask);
			if (error) throw std::system_error(error, std::generic_category(), "read firmware thread signal mask");
			stack_t stack{};
			stack.ss_sp   = signal_stack;
			stack.ss_size = sizeof(signal_stack);
			if (sigaltstack(&stack, nullptr) != 0)
				throw std::system_error(errno, std::generic_category(), "install firmware signal stack");
			// The SDK switches libc's random() state during a firmware grant.
			// Save the host pointer once so a recovered grant can restore it.
			host_random_state = initstate(1, scratch_random_state, sizeof(scratch_random_state));
			setstate(host_random_state);
			prepared = true;
		}
	} // namespace

	bool run_guarded(void (*call)(void*), void* argument, Capture& capture) {
		std::call_once(installed, install);
		prepare_thread();
		const uint32_t limit = watchdog_limit_ms();
		Guard          guard{{}, capture, limit ? watchdog::next_call() : 0, 0};
		capture.frame_count = 0;
		current             = &guard;
		if (sigsetjmp(guard.jump, 0) == 0) {
			if (limit) {
				std::lock_guard lock(interrupt_mutex);
				watched_thread = pthread_self();
				watchdog::watch(guard.call);
			}
			guard.active = 1;
			call(argument);
			guard.active = 0;
		} else {
			pthread_sigmask(SIG_SETMASK, &outside_mask, nullptr);
			setstate(host_random_state);
		}
		watchdog::watch(0);
		current = nullptr;
		return !capture.frame_count;
	}
} // namespace firmware::crash
