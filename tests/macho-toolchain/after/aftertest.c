/* dispatch_after on the main queue (dispatch_main parks the thread in sigsuspend): the block has to run once after 2 s; launchservicesd re-arms its port monitor with it and looped at 500000 calls a second */
typedef void *dq;
extern dq _dispatch_main_q;
extern void dispatch_after(unsigned long long when, dq q, void (^b)(void));
extern unsigned long long dispatch_time(unsigned long long when, long long delta);
extern void dispatch_main(void);
extern long write(int, const void *, unsigned long);
extern void exit(int);
extern unsigned long long mach_absolute_time(void);
static int runs;
static unsigned long long t0;
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }
static void num(const char *p, long v) { char b[24]; int k = 23; b[23] = 0; if (v == 0) b[--k] = '0'; while (v > 0) { b[--k] = '0' + v % 10; v /= 10; } out(p); out(b + k); out("\n"); }
static void again(void);
static void (^blk)(void);

static void
again(void)
{
	runs++;
	if (runs == 1) num("aftertest: first run after ms: ", (long)((mach_absolute_time() - t0) / 1000000));
	if (runs < 5)
		dispatch_after(dispatch_time(0, 1000000000LL), &_dispatch_main_q, blk);
	else {
		num("aftertest: runs ", runs);
		num("aftertest: total ms: ", (long)((mach_absolute_time() - t0) / 1000000));
		exit(0);
	}
}

int
main(void)
{
	t0 = mach_absolute_time();
	blk = ^{ again(); };
	dispatch_after(dispatch_time(0, 2000000000LL), &_dispatch_main_q, blk);
	dispatch_main();
	return 0;
}
