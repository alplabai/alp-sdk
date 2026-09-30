/* libc_min.c -- memcpy/memset for payloads linked -nostdlib: GCC emits calls
 * to them for struct copies and src/ipc/tr_mbox.c uses memset. Byte loops
 * (callers are small structs; build with -fno-tree-loop-distribute-patterns
 * so these don't compile into calls to themselves). */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char       *d = dst;
	const unsigned char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}
