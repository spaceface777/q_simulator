#define _COSMO_SOURCE
#include "../../src/firmware/crash.h"
#include "../../src/firmware/inprocess_backend.h"

#include <gtest/gtest.h>

#include <csignal>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
#include <libc/dce.h>
#include <libc/nt/enum/pageflags.h>
#include <libc/nt/memory.h>
#endif

namespace {
	int bind(uint32_t, uint8_t, const qsim_env_entry_t*, size_t) {
		return 0;
	}

	int healthy(uint64_t, uint64_t end, const quercus_mailbox_t*, quercus_mailbox_t* output, uint64_t*) {
		const uint8_t data = static_cast<uint8_t>(end);
		return quercus_mailbox_append(output, &data, sizeof(data));
	}

	__attribute__((noinline)) int fault(uint64_t, uint64_t, const quercus_mailbox_t*, quercus_mailbox_t* output,
										uint64_t*) {
		const uint8_t partial = 123;
		quercus_mailbox_append(output, &partial, sizeof(partial));
		static volatile uintptr_t address         = 8;
		*reinterpret_cast<volatile int*>(address) = 1;
		return 0;
	}

	__attribute__((noinline)) void hang(void*) {
		for (;;) asm volatile("" ::: "memory");
	}

	volatile bool stop_recursing = false;

	__attribute__((noinline)) void recurse() {
		volatile char frame[2048];
		frame[0] = 1;
		if (!stop_recursing) recurse();
		stop_recursing = frame[0] != 0;
	}

	void overflow(void* stack_top) {
#if defined(__aarch64__)
		asm volatile("mov sp, %0; blr %1; brk #0" : : "r"(stack_top), "r"(&recurse) : "memory");
#else
		asm volatile("mov %0, %%rsp; call *%1; ud2" : : "r"(stack_top), "r"(&recurse) : "memory");
#endif
		__builtin_unreachable();
	}

	constexpr qsim_image_api_t broken{QSIM_IMAGE_ABI_VERSION, bind, fault};
	constexpr qsim_image_api_t working{QSIM_IMAGE_ABI_VERSION, bind, healthy};

	TEST(CrashContainment, DiscardsOutputAndContinuesHealthyGrants) {
		using module::ModuleId;
		const std::array           bindings{firmware::ImageBinding{ModuleId{1}, &broken},
											firmware::ImageBinding{ModuleId{2}, &working}};
		firmware::InProcessBackend backend(bindings, 1, {});
		std::array grants{firmware::ExecutionGrant{.module = ModuleId{1}, .granted = true, .delta_us = 1},
						  firmware::ExecutionGrant{.module = ModuleId{2}, .granted = true, .delta_us = 1}};

		backend.execute_epoch(grants);
		ASSERT_TRUE(grants[0].crash);
		EXPECT_EQ(quercus_mailbox_count(&grants[0].outbound), 0);
		EXPECT_FALSE(grants[1].crash);
		EXPECT_EQ(quercus_mailbox_count(&grants[1].outbound), 1);
		const auto report = firmware::crash::report(*grants[0].crash, 1, 0, grants[0].module_time_us);
		EXPECT_NE(report.find("test_crash_containment.cpp:"), std::string::npos) << report;

		grants[0].granted = false;
		backend.execute_epoch(grants);
		EXPECT_FALSE(grants[0].crash);
		unsigned       size = 0;
		const uint8_t* data = quercus_mailbox_read(&grants[1].outbound, 0, &size);
		ASSERT_EQ(size, 1);
		ASSERT_NE(data, nullptr);
		EXPECT_EQ(*data, 2);
	}

	TEST(CrashContainment, WatchdogInterruptsAndReportsGrant) {
		namespace crash         = firmware::crash;
		const uint32_t previous = crash::watchdog_limit_ms();
		crash::set_watchdog_limit_ms(30);
		crash::Capture capture;
		const bool     returned = crash::run_guarded(hang, nullptr, capture);
		crash::set_watchdog_limit_ms(previous);
		ASSERT_FALSE(returned);
		ASSERT_EQ(capture.cause, crash::Cause::HANG);
		const auto report = crash::report(capture, 1, 0, 1);
		EXPECT_NE(report.find("test_crash_containment.cpp:"), std::string::npos) << report;

		crash::Capture next;
		EXPECT_TRUE(crash::run_guarded([](void*) {}, nullptr, next));
	}

	TEST(CrashContainment, RecoversFromCoroutineStackOverflow) {
		namespace crash                  = firmware::crash;
		constexpr std::size_t stack_size = 256 * 1024;
		const std::size_t     page       = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
		auto*                 stack =
			static_cast<char*>(mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		ASSERT_NE(stack, MAP_FAILED);
		// Leave room below the guard for exception delivery on Windows.
		char* guard = stack + 64 * 1024;
		bool  protected_stack;
#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
		if (IsWindows()) {
			uint32_t old;
			protected_stack = VirtualProtect(guard, page, kNtPageReadwrite | kNtPageGuard, &old);
		} else
#endif
		{
			protected_stack = mprotect(guard, page, PROT_NONE) == 0;
		}
		crash::Capture capture;
		const bool     returned = !protected_stack || crash::run_guarded(overflow, stack + stack_size, capture);
		EXPECT_EQ(munmap(stack, stack_size), 0);
		ASSERT_TRUE(protected_stack);
		ASSERT_FALSE(returned);
#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
		if (IsWindows()) {
			EXPECT_EQ(capture.cause, crash::Cause::STACK_OVERFLOW);
		}
#endif
		const auto report = crash::report(capture, 1, 0, 1);
		EXPECT_NE(report.find("test_crash_containment.cpp:"), std::string::npos) << report;

		crash::Capture next;
		EXPECT_TRUE(crash::run_guarded([](void*) {}, nullptr, next));
	}
} // namespace
