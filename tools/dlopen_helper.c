// Cosmopolitan maps this glibc executable into its own process. Its argument
// is the address of the callback that receives the native loader functions.
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>

int main(int argc, char** argv) {
	if (argc != 2) return 1;
	char*           end;
	const uintptr_t address = strtoull(argv[1], &end, 10);
	if (!address || *end) return 2;
	void* functions[] = {dlopen, dlsym, dlclose, dlerror};
	return ((int (*)(void**))address)(functions);
}
