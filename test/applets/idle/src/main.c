// SPDX-License-Identifier: Apache-2.0
// Sends heartbeats and does nothing else, for scenarios about the world rather than an application.
// HEARTBEATS, if set, stops the heartbeats after that many, so the supervisor stops the module.
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdlib.h>

#include <quercus/rpc.h>

LOG_MODULE_REGISTER(idle, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_MS 50

int main(void) {
	rpc_init();
	while (rpc_wait_ready(K_SECONDS(1)) != 0) {
	}
	const char* limit_text = getenv("HEARTBEATS");
	const long  limit      = limit_text != NULL ? strtol(limit_text, NULL, 10) : -1;
	LOG_INF("ready");

	for (long sent = 0; limit < 0 || sent < limit; sent++) {
		rpc_call(CMD_HEARTBEAT, NULL, 0, NULL, NULL, NULL, K_MSEC(200));
		k_msleep(HEARTBEAT_PERIOD_MS);
	}
	LOG_INF("stopped heartbeats after %ld", limit);
	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
