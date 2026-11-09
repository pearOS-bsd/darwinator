/* probe3: the raw bytes of the old statfs (syscall 157) and statfs64 (345) on a real Mac */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
static void dump(const char *n, long r, unsigned char *b, int len)
{
	printf("%s r=%ld\n", n, r);
	for (int i = 0; i < len; i += 16) { printf("%04x:", i); for (int j = i; j < i + 16 && j < len; j++) printf(" %02x", b[j]); printf("\n"); }
}
int main(void)
{
	static unsigned char a[4096], b[4096];
	memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
	long r1 = syscall(157, "/", a), r2 = syscall(345, "/", b);
	int last1 = 0, last2 = 0;
	for (int i = 0; i < 4096; i++) { if (a[i] != 0xAA) last1 = i + 1; if (b[i] != 0xAA) last2 = i + 1; }
	printf("157 touched %d bytes, 345 touched %d bytes\n", last1, last2);
	dump("157", r1, a, 128); dump("345", r2, b, 128);
	return 0;
}
