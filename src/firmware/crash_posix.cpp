// Fault and hang containment for application images on macOS and Linux.
//
// Application images run on this thread, on coroutine stacks of their own. A fault while one runs
// arrives here as a signal. The handler records the registers and walks the frame-pointer chain,
// then returns into resume(), which jumps back to run_guarded. The application is abandoned
// mid-execution and never resumes.
// Signals outside application code keep their previous handling.
#include "crash.h"
#include "crash_walk.h"
#include "crash_watchdog.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#include <unwind.h>
#endif

namespace firmware::crash {
	namespace {
		constexpr int WATCHDOG_SIGNAL = SIGUSR2;
		constexpr int FAULT_SIGNALS[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGABRT, SIGSYS};

		struct Guard {
			sigjmp_buf            jump;
			Capture*              capture = nullptr;
			uint64_t              call    = 0;
			volatile sig_atomic_t active  = 0;
		};

		thread_local Guard* current_guard   = nullptr;
		thread_local bool   thread_prepared = false;
		// The thread's signal mask outside application code. Saving it per call would cost a system
		// call on every grant, so a failed call restores this copy instead.
		thread_local sigset_t thread_mask;
		// Where a failed call resumes, outside the signal handler. See redirect().
		thread_local char*    resume_stack      = nullptr;
		constexpr std::size_t RESUME_STACK_SIZE = 64 * 1024;

		std::once_flag    setup_once;
		struct sigaction  previous_actions[NSIG];
		int               probe[2] = {-1, -1};
		std::atomic<bool> application_failed{false};
		// The thread running the watched call, and the call the watchdog asked it to give up.
		pthread_t             watched_thread;
		std::atomic<uint64_t> interrupted_call{0};

		struct Registers {
			uintptr_t pc = 0, fp = 0, sp = 0, lr = 0;
		};

		uintptr_t strip(uintptr_t address) noexcept {
#if defined(__aarch64__)
			// Pointer authentication signs saved return addresses in the unused high bits.
			return address & ((uintptr_t{1} << 48) - 1);
#else
			return address;
#endif
		}

		// Copies memory that may be unmapped without faulting. The kernel reports EFAULT for an
		// unreadable source instead of raising a signal. Async-signal-safe.
		bool read_memory(uintptr_t address, void* out, std::size_t size) noexcept {
			if (probe[1] < 0 || address == 0) return false;
			const ssize_t written = ::write(probe[1], reinterpret_cast<const void*>(address), size);
			if (written <= 0) return false;
			char          discard[64];
			const ssize_t received = ::read(probe[0], written == static_cast<ssize_t>(size) ? out : discard,
											static_cast<std::size_t>(written));
			return written == static_cast<ssize_t>(size) && received == written;
		}

		Registers registers(const void* raw) noexcept {
			const auto* context = static_cast<const ucontext_t*>(raw);
			Registers   result;
#if defined(__APPLE__) && defined(__aarch64__)
			const auto& state = context->uc_mcontext->__ss;
			result            = {__darwin_arm_thread_state64_get_pc(state), __darwin_arm_thread_state64_get_fp(state),
								 __darwin_arm_thread_state64_get_sp(state), __darwin_arm_thread_state64_get_lr(state)};
#elif defined(__APPLE__) && defined(__x86_64__)
			const auto& state = context->uc_mcontext->__ss;
			result            = {state.__rip, state.__rbp, state.__rsp, 0};
#elif defined(__linux__) && defined(__aarch64__)
			const auto& state = context->uc_mcontext;
			result            = {state.pc, state.regs[29], state.sp, state.regs[30]};
#elif defined(__linux__) && defined(__x86_64__)
			const auto* state = context->uc_mcontext.gregs;
			result            = {static_cast<uintptr_t>(state[REG_RIP]), static_cast<uintptr_t>(state[REG_RBP]),
								 static_cast<uintptr_t>(state[REG_RSP]), 0};
#elif defined(__linux__) && defined(__i386__)
			const auto* state = context->uc_mcontext.gregs;
			result            = {static_cast<uintptr_t>(state[REG_EIP]), static_cast<uintptr_t>(state[REG_EBP]),
								 static_cast<uintptr_t>(state[REG_ESP]), 0};
#else
#error "Crash containment does not know this CPU's registers"
#endif
			result.pc = strip(result.pc);
			result.lr = strip(result.lr);
			return result;
		}

		// 1 read, 2 write, 3 instruction fetch, 0 unknown. CPUs that report it say which access failed.
		int access_kind(const void* raw) noexcept {
			const auto* context = static_cast<const ucontext_t*>(raw);
#if defined(__aarch64__)
#if defined(__APPLE__)
			const uint64_t esr = context->uc_mcontext->__es.__esr;
#else
			uint64_t       esr = 0;
			const auto*    at  = reinterpret_cast<const uint8_t*>(context->uc_mcontext.__reserved);
			const uint8_t* end = at + sizeof(context->uc_mcontext.__reserved);
			while (at + 8 <= end) {
				uint32_t magic, size;
				std::memcpy(&magic, at, 4);
				std::memcpy(&size, at + 4, 4);
				if (magic == 0 || size < 8) break;
				if (magic == 0x45535201 && size >= 16) std::memcpy(&esr, at + 8, 8); // ESR_MAGIC
				at += size;
			}
			if (esr == 0) return 0;
#endif
			const uint64_t exception_class = esr >> 26;
			if (exception_class == 0x20 || exception_class == 0x21) return 3;
			if (exception_class == 0x24 || exception_class == 0x25) return (esr & (1u << 6)) ? 2 : 1;
			return 0;
#elif defined(__x86_64__) || defined(__i386__)
#if defined(__APPLE__)
			const uint64_t error = context->uc_mcontext->__es.__err;
#elif defined(__x86_64__)
			const uint64_t error = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_ERR]);
#else
			const uint64_t error = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_ERR]);
#endif
			if (error & 0x10) return 3;
			return (error & 0x2) ? 2 : 1;
#else
			(void)context;
			return 0;
#endif
		}

		void walk(Capture& capture, uintptr_t frame, uintptr_t low) noexcept {
			walk_frames(capture, frame, low, read_memory, strip);
		}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
		// abort() raises SIGABRT inside the C library, and the C library calls it for a failed assert
		// or a damaged heap. A C library built without frame pointers, such as glibc before Ubuntu
		// 24.04, breaks the frame-pointer chain there. The unwind tables step through it instead,
		// and the frame-pointer walk continues where the tables end, in code built without them.
		// SIGABRT is raised synchronously, so the unwinder runs in an ordinary state.
#if defined(__x86_64__)
		constexpr int FRAME_REGISTER = 6; // rbp in DWARF numbering
#else
		constexpr int FRAME_REGISTER = 29; // x29
#endif

		struct Unwound {
			Capture*  capture;
			uintptr_t signal_pc;
			bool      reached = false;
			uintptr_t frame = 0, stack = 0;
		};

		_Unwind_Reason_Code record_frame(_Unwind_Context* context, void* data) {
			auto&           unwound = *static_cast<Unwound*>(data);
			int             before  = 0;
			const uintptr_t pc      = _Unwind_GetIPInfo(context, &before);
			// Frames up to the interrupted instruction are this handler's own.
			if (!unwound.reached) {
				if (pc != unwound.signal_pc) return _URC_NO_REASON;
				unwound.reached = true;
			} else {
				Capture& capture = *unwound.capture;
				if (capture.frame_count == MAX_FRAMES) return _URC_END_OF_STACK;
				capture.frames[capture.frame_count++] = pc;
			}
			unwound.frame = _Unwind_GetGR(context, FRAME_REGISTER);
			unwound.stack = _Unwind_GetCFA(context);
			return _URC_NO_REASON;
		}

		_Unwind_Reason_Code ignore_frame(_Unwind_Context*, void*) {
			return _URC_NO_REASON;
		}

		// Replaces the frame-pointer trace of an abort with the unwound one, if the unwinder finds the
		// signal frame.
		void unwind_abort(Capture& capture, uintptr_t signal_pc) noexcept {
			Capture unwound_capture     = capture;
			unwound_capture.frame_count = 1; // The interrupted instruction, as fill() recorded it
			Unwound unwound{&unwound_capture, signal_pc};
			_Unwind_Backtrace(record_frame, &unwound);
			if (!unwound.reached || unwound_capture.frame_count < 2) return;
			std::size_t omitted = 0, omitted_at = unwound_capture.frame_count;
			if (unwound_capture.frame_count < MAX_FRAMES && unwound.frame != 0) {
				Capture rest{};
				walk(rest, unwound.frame, unwound.stack);
				const std::size_t base = unwound_capture.frame_count;
				for (std::size_t index = 0; index < rest.frame_count && unwound_capture.frame_count < MAX_FRAMES;)
					unwound_capture.frames[unwound_capture.frame_count++] = rest.frames[index++];
				if (rest.omitted) omitted = rest.omitted, omitted_at = base + rest.omitted_at;
			}
			capture.frames      = unwound_capture.frames;
			capture.frame_count = unwound_capture.frame_count;
			capture.omitted     = omitted;
			capture.omitted_at  = omitted ? omitted_at : capture.frame_count;
		}
#else
		void unwind_abort(Capture&, uintptr_t) noexcept {}
#endif

		void fill(Capture& capture, Cause cause, int number, const siginfo_t* info, const void* raw) noexcept {
			const Registers state = registers(raw);
			capture.cause         = cause;
			capture.signal        = number;
			capture.code          = info ? info->si_code : 0;
			capture.stack_pointer = state.sp;
			capture.has_address   = cause == Cause::FAULT && info && (number == SIGSEGV || number == SIGBUS);
			capture.address       = capture.has_address ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
			capture.access        = capture.has_address ? access_kind(raw) : 0;
			capture.caller        = state.lr;
			if (capture.caller == 0) {
				uintptr_t top = 0;
				if (read_memory(state.sp, &top, sizeof(top))) capture.caller = top;
			}
			capture.instruction     = 0;
			capture.has_instruction = read_memory(state.pc, &capture.instruction, 8);
			if (!capture.has_instruction) {
				capture.instruction     = 0;
				capture.has_instruction = read_memory(state.pc, &capture.instruction, 4);
			}
			capture.limit_ms                      = cause == Cause::HANG ? watchdog_limit_ms() : 0;
			capture.frame_count                   = 0;
			capture.frames[capture.frame_count++] = state.pc;
			walk(capture, state.fp, state.sp);
			if (number == SIGABRT) unwind_abort(capture, state.pc);
		}

		// Runs after the handler has returned: the kernel's sigreturn has already restored the signal
		// mask and left the alternate signal stack. Jumping out of the handler directly would leave
		// macOS treating the thread as still on that stack, and the next fault would have no stack.
		[[noreturn]] void resume() {
			siglongjmp(current_guard->jump, 1);
		}

		// Makes the interrupted thread continue in resume(), on a stack of its own, once the handler
		// returns. Returns false if this thread has no resume stack.
		bool redirect(void* raw) noexcept {
			if (resume_stack == nullptr) return false;
			auto*           context = static_cast<ucontext_t*>(raw);
			const uintptr_t top = (reinterpret_cast<uintptr_t>(resume_stack) + RESUME_STACK_SIZE - 64) & ~uintptr_t{15};
#if defined(__APPLE__) && defined(__aarch64__)
			auto& state = context->uc_mcontext->__ss;
			__darwin_arm_thread_state64_set_pc_fptr(state, &resume);
			__darwin_arm_thread_state64_set_lr_fptr(state, nullptr);
			__darwin_arm_thread_state64_set_sp(state, top);
			__darwin_arm_thread_state64_set_fp(state, 0);
#elif defined(__APPLE__) && defined(__x86_64__)
			auto& state = context->uc_mcontext->__ss;
			state.__rip = reinterpret_cast<uintptr_t>(&resume);
			state.__rsp = top - 8; // As if resume() had been called
			state.__rbp = 0;
#elif defined(__linux__) && defined(__aarch64__)
			auto& state    = context->uc_mcontext;
			state.pc       = reinterpret_cast<uintptr_t>(&resume);
			state.sp       = top;
			state.regs[29] = 0;
			state.regs[30] = 0;
#elif defined(__linux__) && defined(__x86_64__)
			auto* state    = context->uc_mcontext.gregs;
			state[REG_RIP] = static_cast<greg_t>(reinterpret_cast<uintptr_t>(&resume));
			state[REG_RSP] = static_cast<greg_t>(top - 8);
			state[REG_RBP] = 0;
#elif defined(__linux__) && defined(__i386__)
			auto* state    = context->uc_mcontext.gregs;
			state[REG_EIP] = static_cast<greg_t>(reinterpret_cast<uintptr_t>(&resume));
			state[REG_ESP] = static_cast<greg_t>(top - 4);
			state[REG_EBP] = 0;
#endif
			return true;
		}

		void say(const char* text) noexcept {
			ssize_t ignored = ::write(STDERR_FILENO, text, std::strlen(text));
			(void)ignored;
		}

		void handle(int number, siginfo_t* info, void* raw) {
			const int saved_errno = errno;
			Guard*    guard       = current_guard;

			if (number == WATCHDOG_SIGNAL) {
				// A late watchdog signal for a call that has already returned is ignored.
				if (guard && guard->active && guard->call == interrupted_call.load(std::memory_order_acquire)) {
					guard->active = 0;
					fill(*guard->capture, Cause::HANG, number, info, raw);
					if (redirect(raw)) return;
					siglongjmp(guard->jump, 1);
				}
				errno = saved_errno;
				return;
			}

			if (guard && guard->active) {
				// Cleared first: a fault in this handler then takes the path below.
				guard->active = 0;
				fill(*guard->capture, Cause::FAULT, number, info, raw);
				if (redirect(raw)) return;
				siglongjmp(guard->jump, 1);
			}

			// The simulator itself failed outside application code. Restore the previous handling and
			// let the fault happen again, so that the usual crash report and exit status follow.
			if (application_failed.load(std::memory_order_relaxed))
				say("\nThe simulator crashed outside application code, after an application crashed. That "
					"application may have damaged memory it shares with the simulator, such as the C "
					"library's heap.\n");
			else say("\nThe simulator crashed outside application code. This is a simulator bug.\n");
			sigaction(number, &previous_actions[number], nullptr);
			if (number == SIGTRAP || !info || info->si_code <= 0) raise(number);
			errno = saved_errno;
		}

		void interrupt(uint64_t call) {
			interrupted_call.store(call, std::memory_order_release);
			pthread_kill(watched_thread, WATCHDOG_SIGNAL);
		}

		void install() {
			if (::pipe(probe) == 0) {
				for (const int fd : probe) {
					::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
					::fcntl(fd, F_SETFD, FD_CLOEXEC);
				}
			} else {
				probe[0] = probe[1] = -1;
			}

			struct sigaction action{};
			action.sa_sigaction = handle;
			// SA_NODEFER: a fault inside the handler is delivered again, then takes the restore path.
			action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
			sigemptyset(&action.sa_mask);
			for (const int number : FAULT_SIGNALS) sigaction(number, &action, &previous_actions[number]);

			action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
			sigaction(WATCHDOG_SIGNAL, &action, &previous_actions[WATCHDOG_SIGNAL]);

			watchdog::start(interrupt);
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
			// The unwinder finds and caches the loaded objects' tables on first use. Do that here,
			// not in a signal handler.
			_Unwind_Backtrace(ignore_frame, nullptr);
#endif
		}

		// A stack overflow faults on the guard page below the stack, so the handler needs a stack
		// of its own on every thread that runs application code.
		void prepare_thread() {
			if (thread_prepared) return;
			thread_prepared = true;
			pthread_sigmask(SIG_SETMASK, nullptr, &thread_mask);
			void* resume = mmap(nullptr, RESUME_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			if (resume != MAP_FAILED) resume_stack = static_cast<char*>(resume);
			stack_t existing{};
			if (sigaltstack(nullptr, &existing) == 0 && !(existing.ss_flags & SS_DISABLE) &&
				existing.ss_size >= 64 * 1024)
				return;
			constexpr std::size_t size   = 256 * 1024;
			void*                 memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			if (memory == MAP_FAILED) return;
			stack_t stack{};
			stack.ss_sp    = memory;
			stack.ss_size  = size;
			stack.ss_flags = 0;
			if (sigaltstack(&stack, nullptr) != 0) munmap(memory, size);
		}
	} // namespace

	bool run_guarded(void (*call)(void*), void* argument, Capture& capture) noexcept {
		std::call_once(setup_once, install);
		prepare_thread();

		Guard guard;
		guard.capture = &capture;
		guard.call    = watchdog::next_call();

		Guard* volatile const outer = current_guard;
		current_guard               = &guard;
		watched_thread              = pthread_self();
		watchdog::watch(guard.call);

		if (sigsetjmp(guard.jump, 0) == 0) {
			guard.active = 1;
			call(argument);
			guard.active  = 0;
			current_guard = outer;
			watchdog::watch(outer ? outer->call : 0);
			return true;
		}

		// Arrived through resume(), or straight from the handler if this thread has no resume stack.
		// Signals may still be blocked: abort() blocks all but SIGABRT before raising it.
		pthread_sigmask(SIG_SETMASK, &thread_mask, nullptr);
		application_failed.store(true, std::memory_order_relaxed);
		current_guard = outer;
		watchdog::watch(outer ? outer->call : 0);
		return false;
	}

	__attribute__((noinline)) void capture_here(Capture& capture) noexcept {
		std::call_once(setup_once, install);
		const auto frame    = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
		capture.cause       = Cause::ENDED;
		capture.frame_count = 0;
		walk(capture, frame, frame);
		// The walk starts in this frame, so it must stay alive. As a tail call, the walk would
		// release the frame first and overwrite its record (GCC on arm64 does).
		asm volatile("" ::: "memory");
	}

} // namespace firmware::crash
