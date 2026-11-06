/* Built with CONFIG_QUERCUS_SIM_CPU_TIME, so its code costs simulated time. CPU_MODE selects
 * what the module given by CPU_MODULE does; every other module only sends heartbeats.
 *
 *   work  Runs a fixed computation and shows how many simulated milliseconds it took as the
 *         LED colour.
 *   spin  Loops forever without calling Zephyr, at a higher priority than the heartbeat. */
#include <quercus/rpc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>

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

static volatile uint32_t sink;

static void set_led(uint32_t color) {
	(void)rpc_call(CMD_LED_SET_COLOR, &color, sizeof(color), NULL, NULL, NULL, K_MSEC(200));
}

int main(void) {
	if (rpc_init() != 0) return 0;
	while (rpc_wait_ready(K_SECONDS(1)) != 0) {
	}
	k_thread_start(heartbeat_id);
	set_led(0x000001);

	const char* mode   = getenv("CPU_MODE");
	const char* module = getenv("CPU_MODULE");
	const char* self   = getenv("QUERCUS_MODULE_ID");
	if (!mode || !module || !self || strcmp(module, self) != 0) {
		for (;;) k_sleep(K_FOREVER);
	}

	k_msleep(500);
	if (strcmp(mode, "work") == 0) {
		const int64_t start = k_uptime_get();
		for (uint32_t i = 0; i < 1000000u; ++i) sink = sink * 31u + i;
		set_led((uint32_t)(k_uptime_get() - start));
	} else if (strcmp(mode, "spin") == 0) {
		for (;;) sink = sink + 1u;
	}
	for (;;) k_sleep(K_FOREVER);
	return 0;
}
