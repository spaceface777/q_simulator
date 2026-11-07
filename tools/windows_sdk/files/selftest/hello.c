#include <stdio.h>

int main(void)
{
	printf("hello from C, %u-bit pointers\n", (unsigned)(sizeof(void *) * 8));
	return sizeof(void *) == 4 ? 0 : 1;
}
