// SPDX-License-Identifier: Apache-2.0
#include <quercus/rpc_wire.h>
#include <quercus/sim_instance.h>

#include <stdio.h>

extern const qsim_image_api_t __start_qsim_instances[];
extern const qsim_image_api_t __stop_qsim_instances[];

#define CHECK(condition)                                                     \
	do {                                                                     \
		if (!(condition)) {                                                  \
			fprintf(stderr, "two_instances:%d: %s\n", __LINE__, #condition); \
			return 1;                                                        \
		}                                                                    \
	} while (0)

int main(void) {
	const qsim_image_api_t* instances = __start_qsim_instances;
	CHECK(__stop_qsim_instances - instances == 2);
	for (unsigned i = 0; i < 2; ++i) {
		CHECK(instances[i].abi_version == QSIM_IMAGE_ABI_VERSION);
		CHECK(instances[i].bind(100 + i) == 0);
	}

	quercus_mailbox_t input[2]        = {0};
	unsigned          requested_id[2] = {0};
	unsigned          reported_id[2]  = {0};
	for (uint64_t until = 1000; until <= 2000000; until += 1000) {
		for (unsigned i = 0; i < 2; ++i) {
			quercus_mailbox_t output;
			uint64_t          next;
			CHECK(instances[i].run(until - 1000, until, &input[i], &output, &next) == 0);
			CHECK(next > until);
			quercus_mailbox_clear(&input[i]);
			for (unsigned j = 0; j < quercus_mailbox_count(&output); ++j) {
				unsigned       size;
				const uint8_t* bytes = quercus_mailbox_read(&output, j, &size);
				CHECK(size >= sizeof(rpc_message_t));
				rpc_message_t request;
				memcpy(&request, bytes, sizeof(request));
				if (request.type != MT_REQ) continue;
				if (request.cmd == CMD_LOG) {
					const char* expected = i == 0 ? "module id=1 " : "module id=7 ";
					if (memmem(bytes + sizeof(request), size - sizeof(request), expected, strlen(expected)))
						++reported_id[i];
				}
				if (request.flags & RPC_FLAG_NON_CON) continue;
				rpc_message_t response = {
					.type    = MT_RESP,
					.cmd     = request.cmd,
					.call_id = request.call_id,
				};
				uint8_t frame[sizeof(response) + 1];
				if (request.cmd == CMD_GET_OWN_ID) {
					++requested_id[i];
					response.payload_len    = 1;
					frame[sizeof(response)] = i == 0 ? 1 : 7;
				}
				memcpy(frame, &response, sizeof(response));
				CHECK(quercus_mailbox_append(&input[i], frame, sizeof(response) + response.payload_len) == 0);
			}
		}
	}
	for (unsigned i = 0; i < 2; ++i) {
		CHECK(requested_id[i] == 1);
		CHECK(reported_id[i] == 1);
	}
	puts("Two demo_app instances received and reported different module IDs");
	return 0;
}
