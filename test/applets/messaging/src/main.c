// SPDX-License-Identifier: Apache-2.0
// For a layout of modules 1 and 2: asks the platform for its module ID, then sends greetings to the
// other module and logs the first greeting it receives. GREETINGS (default 4) sets how many, and
// SEND_PERIOD_MS (default 250) how far apart. The platform allows 5 network requests per second.
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <quercus/rpc.h>

LOG_MODULE_REGISTER(messaging, LOG_LEVEL_INF);

#define RPC_TIMEOUT K_MSEC(200)

static K_SEM_DEFINE(greeted, 0, 1);
static atomic_t greeting_seen;
static char     received[64];

void rpc_handle_event(rpc_event_t event, int16_t status, const void* payload, uint16_t len) {
	ARG_UNUSED(status);
	// Only the first greeting is kept.
	if (event != EVENT_MESSAGE_RECEIVED || atomic_set(&greeting_seen, 1) != 0) return;
	const size_t size = MIN(len, sizeof(received) - 1);
	memcpy(received, payload, size);
	received[size] = '\0';
	k_sem_give(&greeted);
}

static void heartbeat(void* a, void* b, void* c) {
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		rpc_call(CMD_HEARTBEAT, NULL, 0, NULL, NULL, NULL, RPC_TIMEOUT);
		k_msleep(50);
	}
}

K_THREAD_DEFINE(heartbeat_thread, 1024, heartbeat, NULL, NULL, NULL, 4, 0, 0);

int main(void) {
	rpc_init();
	while (rpc_wait_ready(K_SECONDS(1)) != 0) {
	}
	uint8_t  own_id = 0;
	uint16_t size   = sizeof(own_id);
	int      status = 0;
	if (rpc_call(CMD_GET_OWN_ID, NULL, 0, &own_id, &size, &status, RPC_TIMEOUT) != 0 || status != 0) {
		LOG_ERR("could not get the module ID: status %d", status);
		return 0;
	}
	LOG_INF("module id=%u", own_id);
	const uint32_t events = EVENT_MESSAGE_RECEIVED;
	rpc_call(CMD_SUBSCRIBE_TO_EVENT, &events, sizeof(events), NULL, NULL, &status, RPC_TIMEOUT);

	uint8_t packet[32];
	packet[0]               = own_id == 1 ? 2 : 1;
	const int   length      = snprintf((char*)packet + 1, sizeof(packet) - 1, "hello from %u", own_id);
	const char* count_text  = getenv("GREETINGS");
	const char* period_text = getenv("SEND_PERIOD_MS");
	const long  count       = count_text != NULL ? strtol(count_text, NULL, 10) : 4;
	const long  period_ms   = period_text != NULL ? strtol(period_text, NULL, 10) : 250;
	for (long sent = 1; sent <= count; sent++) {
		rpc_call(CMD_SEND_PACKET, packet, (uint16_t)(length + 1), NULL, NULL, &status, RPC_TIMEOUT);
		if (status != 0) LOG_WRN("greeting %ld refused with status %d", sent, status);
		if (k_sem_take(&greeted, K_MSEC(period_ms)) == 0) LOG_INF("received \"%s\"", received);
	}
	LOG_INF("sent %ld greetings", count);
	return 0;
}
