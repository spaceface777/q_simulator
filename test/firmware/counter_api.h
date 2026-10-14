#pragma once

#include <stdint.h>

/* SDK-independent fixture: the loader reads only the version. The operation table
 * has the image ABI's three callbacks; configuration and grants stay opaque. */
typedef struct CounterFrame {
	const uint8_t* data;
	uint32_t       size;
} CounterFrame;

typedef struct CounterResult {
	int                 outcome;
	uint64_t            next_wake_us;
	const CounterFrame* outbound_frames;
	uint32_t            outbound_count;
	const char*         diagnostic;
} CounterResult;

typedef struct CounterApi {
	uint32_t abi_version;
	int (*bind)(const void*);
	int (*run)(const void*, CounterResult*);
	int (*destroy)(void);
} CounterApi;
