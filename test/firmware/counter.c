#include "counter_api.h"

#ifndef COUNTER_ABI_VERSION
#define COUNTER_ABI_VERSION 10
#endif

static uint64_t counter;

static int bind_counter(const void* config) {
	(void)config;
	return 0;
}

static int run_counter(const void* grant, CounterResult* result) {
	(void)grant;
	*result = (CounterResult){.next_wake_us = ++counter};
	return 0;
}

static int destroy_counter(void) {
	return 0;
}

#ifdef COUNTER_MISSING_ENTRY
__attribute__((visibility("default"))) const CounterApi* another_entry(void) {
#else
__attribute__((visibility("default"))) const CounterApi* qsim_image_api(void) {
#endif
#ifdef COUNTER_NULL_API
	return 0;
#else
	static const CounterApi api = {COUNTER_ABI_VERSION, bind_counter, run_counter, destroy_counter};
	return &api;
#endif
}
