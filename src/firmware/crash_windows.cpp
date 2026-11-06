// Fault and hang containment for application images on Windows.
//
// A vectored exception handler records a fault in application code, then resumes the thread in a
// function on a separate stack. That function restores the thread block that the coroutine switch
// changed, and jumps back to run_guarded. The watchdog suspends a hung thread and redirects it the
// same way. abort() from the C runtime arrives as SIGABRT through the CRT's signal handler.
//
// A stack overflow cannot be contained here: Windows delivers the exception on the exhausted stack.
#include "crash.h"
#include "crash_walk.h"
#include "crash_watchdog.h"

#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace firmware::crash {
	namespace {
		constexpr std::size_t RESUME_STACK_SIZE = 64 * 1024;

		// The fields of the thread block that the coroutine switch exchanges.
		struct ThreadBlock {
			void* exception_list     = nullptr;
			void* stack_base         = nullptr;
			void* stack_limit        = nullptr;
			void* deallocation_stack = nullptr;
		};

		void** deallocation_slot(NT_TIB* block) {
#if defined(__i386__)
			return reinterpret_cast<void**>(reinterpret_cast<char*>(block) + 0xe0c);
#elif defined(__x86_64__) || defined(__aarch64__)
			return reinterpret_cast<void**>(reinterpret_cast<char*>(block) + 0x1478);
#else
			return nullptr;
#endif
		}

		ThreadBlock save_thread_block() {
			auto*       block = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
			ThreadBlock saved{block->ExceptionList, block->StackBase, block->StackLimit, nullptr};
			if (void** slot = deallocation_slot(block)) saved.deallocation_stack = *slot;
			return saved;
		}

		void restore_thread_block(const ThreadBlock& saved) {
			auto* block          = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
			block->ExceptionList = static_cast<EXCEPTION_REGISTRATION_RECORD*>(saved.exception_list);
			block->StackBase     = saved.stack_base;
			block->StackLimit    = saved.stack_limit;
			if (void** slot = deallocation_slot(block)) *slot = saved.deallocation_stack;
		}

		struct Guard {
			void*         jump[5] = {}; // __builtin_setjmp
			Capture*      capture = nullptr;
			uint64_t      call    = 0;
			volatile LONG active  = 0;
			ThreadBlock   block;
			char*         resume_stack = nullptr; // The guarded thread's; the watchdog redirects to it
		};

		thread_local Guard* current_guard = nullptr;
		thread_local char*  resume_stack  = nullptr;

		std::once_flag setup_once;

		std::atomic<Guard*> watched_guard{nullptr};
		std::atomic<HANDLE> watched_thread{nullptr};

		struct Registers {
			uintptr_t pc = 0, fp = 0, sp = 0, lr = 0;
		};

		Registers registers(const CONTEXT& context) {
#if defined(__i386__)
			return {context.Eip, context.Ebp, context.Esp, 0};
#elif defined(__x86_64__)
			return {context.Rip, context.Rbp, context.Rsp, 0};
#elif defined(__aarch64__)
			return {context.Pc, context.Fp, context.Sp, context.Lr};
#else
#error "Crash containment does not know this CPU's registers"
#endif
		}

		bool read_memory(uintptr_t address, void* out, std::size_t size) noexcept {
			SIZE_T copied = 0;
			return address != 0 &&
				   ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &copied) &&
				   copied == size;
		}

		uintptr_t keep(uintptr_t address) noexcept {
			return address;
		}

		void walk(Capture& capture, uintptr_t frame, uintptr_t low) noexcept {
			walk_frames(capture, frame, low, read_memory, keep);
		}

		void fill(Capture& capture, Cause cause, const CONTEXT& context, const EXCEPTION_RECORD* record) noexcept {
			const Registers state = registers(context);
			capture.cause         = cause;
			capture.stack_pointer = state.sp;
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
			capture.limit_ms = cause == Cause::HANG ? watchdog_limit_ms() : 0;
			if (record) {
				const DWORD code = record->ExceptionCode;
				capture.code     = static_cast<int>(code);
				switch (code) {
				case EXCEPTION_ACCESS_VIOLATION:
				case EXCEPTION_IN_PAGE_ERROR:
				case EXCEPTION_STACK_OVERFLOW:
				case EXCEPTION_DATATYPE_MISALIGNMENT: capture.signal = SIGSEGV; break;
				case EXCEPTION_ILLEGAL_INSTRUCTION:
				case EXCEPTION_PRIV_INSTRUCTION:      capture.signal = SIGILL; break;
				case EXCEPTION_BREAKPOINT:            capture.signal = SIGILL; break;
				default:                              capture.signal = SIGFPE; break;
				}
				capture.has_address = (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
									  record->NumberParameters >= 2;
				if (capture.has_address) {
					capture.address        = static_cast<uintptr_t>(record->ExceptionInformation[1]);
					const ULONG_PTR access = record->ExceptionInformation[0];
					capture.access         = access == 0 ? 1 : access == 1 ? 2 : access == 8 ? 3 : 0;
				}
			}
			capture.frame_count                   = 0;
			capture.frames[capture.frame_count++] = state.pc;
			walk(capture, state.fp, state.sp);
		}

		[[noreturn]] void resume() {
			Guard* guard = current_guard;
			restore_thread_block(guard->block);
			__builtin_longjmp(guard->jump, 1);
		}

		// Continues the thread in resume(), on a stack that the failure cannot have damaged.
		void redirect(CONTEXT& context, char* stack) {
			uintptr_t top = (reinterpret_cast<uintptr_t>(stack) + RESUME_STACK_SIZE - 64) & ~uintptr_t{15};
#if defined(__i386__)
			context.Esp = static_cast<DWORD>(top - 4); // As if resume() had been called
			context.Ebp = 0;
			context.Eip = reinterpret_cast<DWORD>(&resume);
#elif defined(__x86_64__)
			context.Rsp = top - 8 - 32; // Return address slot and the callee's shadow space
			context.Rbp = 0;
			context.Rip = reinterpret_cast<DWORD64>(&resume);
#elif defined(__aarch64__)
			context.Sp = top;
			context.Fp = 0;
			context.Lr = 0;
			context.Pc = reinterpret_cast<DWORD64>(&resume);
#endif
		}

		bool fatal(DWORD code) {
			switch (code) {
			case EXCEPTION_ACCESS_VIOLATION:
			case EXCEPTION_IN_PAGE_ERROR:
			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_INT_OVERFLOW:
			case EXCEPTION_DATATYPE_MISALIGNMENT:
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			case EXCEPTION_BREAKPOINT:
			case EXCEPTION_FLT_DIVIDE_BY_ZERO:
			case EXCEPTION_FLT_INVALID_OPERATION:
			case EXCEPTION_FLT_OVERFLOW:
			case EXCEPTION_FLT_UNDERFLOW:
			case EXCEPTION_FLT_INEXACT_RESULT:
			case EXCEPTION_FLT_DENORMAL_OPERAND:
			case EXCEPTION_FLT_STACK_CHECK:       return true;
			default:                              return false;
			}
		}

		LONG CALLBACK on_exception(EXCEPTION_POINTERS* pointers) {
			Guard* guard = current_guard;
			if (guard == nullptr || !guard->active || !fatal(pointers->ExceptionRecord->ExceptionCode))
				return EXCEPTION_CONTINUE_SEARCH;
			guard->active = 0;
			fill(*guard->capture, Cause::FAULT, *pointers->ContextRecord, pointers->ExceptionRecord);
			redirect(*pointers->ContextRecord, guard->resume_stack);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		void on_abort(int) {
			std::signal(SIGABRT, on_abort);
			Guard* guard = current_guard;
			if (guard == nullptr || !guard->active) {
				std::signal(SIGABRT, SIG_DFL);
				std::raise(SIGABRT);
				return;
			}
			guard->active       = 0;
			Capture& capture    = *guard->capture;
			capture.cause       = Cause::FAULT;
			capture.signal      = SIGABRT;
			capture.frame_count = 0;
			const auto frame    = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
			walk(capture, frame, frame);
			restore_thread_block(guard->block);
			__builtin_longjmp(guard->jump, 1);
		}

		// Runs on the watchdog thread. The guarded thread is suspended while its context changes.
		void interrupt(uint64_t call) {
			HANDLE thread = watched_thread.load(std::memory_order_acquire);
			if (thread == nullptr || SuspendThread(thread) == static_cast<DWORD>(-1)) return;
			Guard*  guard = watched_guard.load(std::memory_order_acquire);
			CONTEXT context{};
			context.ContextFlags = CONTEXT_FULL;
			// The call may have returned before the thread stopped.
			if (guard && guard->call == call && watchdog::watched() == call && guard->active &&
				GetThreadContext(thread, &context)) {
				guard->active = 0;
				fill(*guard->capture, Cause::HANG, context, nullptr);
				redirect(context, guard->resume_stack);
				SetThreadContext(thread, &context);
			}
			ResumeThread(thread);
		}

		void install() {
			AddVectoredExceptionHandler(1, on_exception);
			std::signal(SIGABRT, on_abort);
			// abort() otherwise opens an error-reporting dialog before it raises SIGABRT.
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			watchdog::start(interrupt);
		}
	} // namespace

	bool run_guarded(void (*call)(void*), void* argument, Capture& capture) noexcept {
		std::call_once(setup_once, install);
		if (resume_stack == nullptr) {
			resume_stack =
				static_cast<char*>(VirtualAlloc(nullptr, RESUME_STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			if (resume_stack == nullptr) {
				call(argument);
				return true;
			}
			HANDLE thread = nullptr;
			DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
							THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, 0);
			watched_thread.store(thread, std::memory_order_release);
		}

		Guard guard;
		guard.capture      = &capture;
		guard.call         = watchdog::next_call();
		guard.block        = save_thread_block();
		guard.resume_stack = resume_stack;

		Guard* volatile const outer = current_guard;
		current_guard               = &guard;
		watched_guard.store(&guard, std::memory_order_release);
		watchdog::watch(guard.call);

		if (__builtin_setjmp(guard.jump) == 0) {
			guard.active = 1;
			call(argument);
			guard.active = 0;
			watchdog::watch(outer ? outer->call : 0);
			watched_guard.store(outer, std::memory_order_release);
			current_guard = outer;
			return true;
		}

		watchdog::watch(outer ? outer->call : 0);
		watched_guard.store(outer, std::memory_order_release);
		current_guard = outer;
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
