// SPDX-License-Identifier: Apache-2.0
// Boots, runs for a short while, then dereferences a null pointer. The linked-world tests use it
// to check that the simulator contains the crash and reports it with a source line.
#include <zephyr/kernel.h>

#include <stdint.h>

// noinline keeps a frame for this function, so the report names this file.
__attribute__((noinline)) static void write_through(volatile uint32_t* target) {
	*target = 0xdead;
}

int main(void) {
	k_msleep(20);
	write_through((volatile uint32_t*)(uintptr_t)0);
	return 0;
}
