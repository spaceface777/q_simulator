// SPDX-License-Identifier: Apache-2.0
// Fails in the way that the scenario's CRASH variable names: segfault (the default), abort, hang or
// stack_overflow. The simulator must contain each failure, report it, and keep the other modules
// running.
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(crash, LOG_LEVEL_INF);

// noinline keeps a frame for each function, so that a crash report names this file.
__attribute__((noinline)) static void write_through(volatile uint32_t* target) {
	*target = 0xdead;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"

__attribute__((noinline)) static unsigned recurse(unsigned depth) {
	volatile char frame[512];
	frame[0] = (char)depth;
	// The addition after the call keeps it from becoming a loop.
	return recurse(depth + 1) + frame[0];
}

#pragma GCC diagnostic pop

int main(void) {
	const char* mode = getenv("CRASH");
	if (mode == NULL) mode = "segfault";
	LOG_INF("failing with %s", mode);
	// Lets the deferred logger send the line above before the failure.
	k_msleep(20);

	if (strcmp(mode, "segfault") == 0) {
		write_through((volatile uint32_t*)(uintptr_t)0);
	} else if (strcmp(mode, "abort") == 0) {
		abort();
	} else if (strcmp(mode, "hang") == 0) {
		// Never yields, so only the grant watchdog can end it.
		for (volatile int spin = 0;; spin++) {
		}
	} else if (strcmp(mode, "stack_overflow") == 0) {
		LOG_INF("recursed %u times", recurse(0));
	}
	LOG_ERR("unknown CRASH mode %s", mode);
	return 0;
}
