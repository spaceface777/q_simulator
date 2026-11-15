// SPDX-License-Identifier: Apache-2.0
// Carries the tub at its entry to the left laser. When the reader sees the tag, it turns the LED
// blue, routes the junction left and runs both belts inwards. When the tub breaks the left beam, it
// stops the belts and turns the LED green.
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdbool.h>
#include <stdint.h>

#include <quercus/rpc.h>

LOG_MODULE_REGISTER(conveyor, LOG_LEVEL_INF);

#define RPC_TIMEOUT K_MSEC(200)

#define LED_BLUE  UINT32_C(0x0000ff)
#define LED_GREEN UINT32_C(0x00ff00)
// The junction routes a tub left between 110 and 130 degrees.
#define SERVO_LEFT 120

static K_SEM_DEFINE(tag_detected, 0, 1);
static K_SEM_DEFINE(left_beam_broken, 0, 1);

void rpc_handle_event(rpc_event_t event, int16_t status, const void* payload, uint16_t len) {
	ARG_UNUSED(status);
	ARG_UNUSED(payload);
	ARG_UNUSED(len);
	if (event == EVENT_RFID_DETECT) k_sem_give(&tag_detected);
	if (event == EVENT_LASER_LEFT_DETECT) k_sem_give(&left_beam_broken);
}

static void command(rpc_cmd_t cmd, const void* payload, uint16_t len) {
	int status = 0;
	int result = rpc_call(cmd, payload, len, NULL, NULL, &status, RPC_TIMEOUT);
	if (result < 0 || status != 0) LOG_ERR("command 0x%02x failed: %d, status %d", cmd, result, status);
}

static void set_value(rpc_cmd_t cmd, int32_t value) {
	command(cmd, &value, sizeof(value));
}

static void set_laser(rpc_cmd_t cmd, bool on) {
	const uint8_t value = on;
	command(cmd, &value, sizeof(value));
}

static bool tag_present(void) {
	uint8_t  value  = 0;
	uint16_t size   = sizeof(value);
	int      status = 0;
	return rpc_call(CMD_RFID_CHECK_TAG, NULL, 0, &value, &size, &status, RPC_TIMEOUT) == 0 && status == 0 &&
		   size == sizeof(value) && value == 1;
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
	set_laser(CMD_LASER_LEFT_SET, true);
	set_laser(CMD_LASER_RIGHT_SET, true);
	const uint32_t events = EVENT_RFID_DETECT | EVENT_LASER_LEFT_DETECT;
	command(CMD_SUBSCRIBE_TO_EVENT, &events, sizeof(events));
	LOG_INF("waiting for a tag");

	// A tag already at the reader raised no event.
	if (!tag_present()) k_sem_take(&tag_detected, K_FOREVER);
	LOG_INF("tag detected; carrying it to the left laser");
	set_value(CMD_LED_SET_COLOR, LED_BLUE);
	set_value(CMD_SERVO_ANGLE_SET, SERVO_LEFT);
	set_value(CMD_BELT_SMALL_SET_SPEED, -100);
	set_value(CMD_BELT_BIG_SET_SPEED, 100);

	k_sem_take(&left_beam_broken, K_FOREVER);
	set_value(CMD_BELT_BIG_SET_SPEED, 0);
	set_value(CMD_BELT_SMALL_SET_SPEED, 0);
	set_value(CMD_LED_SET_COLOR, LED_GREEN);
	LOG_INF("tub at the left laser");
	return 0;
}
