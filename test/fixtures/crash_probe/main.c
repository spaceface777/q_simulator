/* Fails at runtime on purpose, for the crash-containment tests. CRASH_MODE selects the failure,
 * CRASH_MODULE the one module that fails. Every other module runs normally and keeps its LED
 * green, which shows that it was not stopped with the failing one. */
#include <quercus/rpc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(crash_probe, LOG_LEVEL_INF);

static void heartbeat(void* a, void* b, void* c) {
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		(void)rpc_call(CMD_HEARTBEAT, NULL, 0, NULL, NULL, NULL, K_MSEC(200));
		k_msleep(50);
	}
}

K_THREAD_DEFINE(heartbeat_id, 1024, heartbeat, NULL, NULL, NULL, 4, 0, SYS_FOREVER_MS);

static int* volatile nowhere;
static volatile int divisor;

static volatile int recursion_limit = -1;

/* Recurses until the stack runs out: the limit is never reached. */
static int __attribute__((noinline)) recurse(volatile int depth) {
	volatile char frame[256];
	frame[0] = (char)depth;
	if (depth == recursion_limit) return frame[0];
	return recurse(depth + 1) + frame[0];
}

static void __attribute__((noinline)) crash_probe_fault(const char* mode) {
	LOG_INF("failing with %s", mode);
	if (strcmp(mode, "null_read") == 0) {
		LOG_INF("read %d", *nowhere);
	} else if (strcmp(mode, "abort") == 0) {
		abort();
	} else if (strcmp(mode, "exit") == 0) {
		exit(3);
	} else if (strcmp(mode, "k_panic") == 0) {
		k_panic();
	} else if (strcmp(mode, "divide") == 0) {
		LOG_INF("quotient %d", 7 / divisor);
	} else if (strcmp(mode, "recursion") == 0) {
		LOG_INF("depth %d", recurse(0));
	} else if (strcmp(mode, "hang") == 0) {
		for (volatile uint32_t spin = 0;; spin = spin + 1) {
		}
	}
}

int main(void) {
	if (rpc_init() != 0) return 0;
	while (rpc_wait_ready(K_SECONDS(1)) != 0) {
	}
	k_thread_start(heartbeat_id);

	const uint32_t green = 0x00ff00;
	(void)rpc_call(CMD_LED_SET_COLOR, &green, sizeof(green), NULL, NULL, NULL, K_MSEC(200));

	const char* mode   = getenv("CRASH_MODE");
	const char* module = getenv("CRASH_MODULE");
	const char* self   = getenv("QUERCUS_MODULE_ID");
	if (mode && module && self && strcmp(module, self) == 0) {
		k_msleep(1000);
		crash_probe_fault(mode);
	}
	for (;;) {
		k_msleep(1000);
	}
	return 0;
}
