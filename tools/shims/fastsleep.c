/* fastsleep.dylib: DYLD_INSERT_LIBRARIES interpose that shortens sleep(n) to ~1 ms.
 * IOSurface's connect loop does sleep(1) x241 waiting for IOSurfaceRoot, which
 * pearOS (no IOKit) never provides; this makes the failure path arrive fast.
 * Build on a Mac: clang -dynamiclib -o fastsleep.dylib fastsleep.c */
#include <time.h>
#include <unistd.h>
static unsigned int my_sleep(unsigned int s) {
	struct timespec ts = { 0, 1000000 };
	nanosleep(&ts, 0);
	return 0;
}
__attribute__((used)) static struct { const void *n, *o; } interp[]
	__attribute__((section("__DATA,__interpose"))) = { { (const void *)my_sleep, (const void *)sleep } };
