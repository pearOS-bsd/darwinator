/* libdispatch against the kernel module: asynchronous work on the global queue, on a serial queue, a timer, a group; built on FreeBSD (clang -fblocks, ld64.lld, no Apple header), run by the real dyld:  prints "dtest: all ok" */
typedef unsigned long u64;
typedef void *dq;
extern dq dispatch_get_global_queue(long ident, unsigned long flags);
extern dq dispatch_queue_create(const char *label, void *attr);
extern void dispatch_async(dq q, void (^blk)(void));
extern void *dispatch_semaphore_create(long v);
extern long dispatch_semaphore_wait(void *s, u64 t);
extern long dispatch_semaphore_signal(void *s);
extern u64 dispatch_time(u64 when, long delta);
extern void dispatch_after(u64 when, dq q, void (^blk)(void));
extern void *dispatch_group_create(void);
extern void dispatch_group_async(void *g, dq q, void (^blk)(void));
extern long dispatch_group_wait(void *g, u64 t);
extern long write(int fd, const void *buf, unsigned long n);
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }
static int fails;
static void check(const char *what, int ok) { out(ok ? "ok   " : "FAIL "); out(what); out("\n"); if (!ok) fails++; }

int
main(void)
{
	static volatile int ran1, ran2, ran3, ran4;
	void *sem = dispatch_semaphore_create(0);
	dq g = dispatch_get_global_queue(0, 0);
	dq serial = dispatch_queue_create("com.pearos.dtest", 0);
	void *grp = dispatch_group_create();

	dispatch_async(g, ^{ ran1 = 1; dispatch_semaphore_signal(sem); });
	check("async on the global queue runs", dispatch_semaphore_wait(sem, dispatch_time(0, 3000000000L)) == 0 && ran1);
	dispatch_async(serial, ^{ ran2 = 1; dispatch_semaphore_signal(sem); });
	check("async on a serial queue runs", dispatch_semaphore_wait(sem, dispatch_time(0, 3000000000L)) == 0 && ran2);
	dispatch_after(dispatch_time(0, 100000000L), g, ^{ ran3 = 1; dispatch_semaphore_signal(sem); });
	check("dispatch_after (a timer, 100 ms) fires", dispatch_semaphore_wait(sem, dispatch_time(0, 3000000000L)) == 0 && ran3);
	dispatch_group_async(grp, g, ^{ ran4 = 1; });
	dispatch_group_async(grp, serial, ^{ ran4 += 1; });
	check("a group of two blocks finishes", dispatch_group_wait(grp, dispatch_time(0, 3000000000L)) == 0 && ran4 == 2);
	out(fails ? "dtest: FAILED\n" : "dtest: all ok\n");
	return (fails != 0);
}
