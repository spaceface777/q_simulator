#include <quercus/rpc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>

int main(void) {
	rpc_init();
	const char* id        = getenv("QUERCUS_MODULE_ID");
	const char* value     = getenv("QSIM_STUB_REFUSE");
	uint8_t     layout_id = 0;
	uint16_t    length    = sizeof(layout_id);
	int         status    = 0;
	int         result    = rpc_call(CMD_GET_OWN_ID, NULL, 0, &layout_id, &length, &status, K_FOREVER);
	char        expected_id[4];
	(void)snprintf(expected_id, sizeof(expected_id), "%u", (unsigned)layout_id);
	bool valid = result == 0 && status == 0 && length == sizeof(layout_id) && id && value &&
				 strcmp(id, expected_id) == 0 && strcmp(value, "6:30000") == 0 && getenv("PATH") == NULL;
	while (true) {
		const char* fresh_id    = getenv("QUERCUS_MODULE_ID");
		const char* fresh_value = getenv("QSIM_STUB_REFUSE");
		valid = valid && fresh_id == id && fresh_value == value && strcmp(fresh_id, expected_id) == 0 &&
				strcmp(fresh_value, "6:30000") == 0 && getenv("PATH") == NULL;
		(void)rpc_call(CMD_HEARTBEAT, NULL, 0, NULL, NULL, &status, K_FOREVER);
		const uint32_t color = valid ? 0x00ff00 : 0xff0000;
		(void)rpc_call(CMD_LED_SET_COLOR, &color, sizeof(color), NULL, NULL, &status, K_FOREVER);
		k_msleep(500);
	}
	return 0;
}
