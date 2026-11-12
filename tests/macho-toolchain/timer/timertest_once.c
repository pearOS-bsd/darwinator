/* a dispatch timer of 2 s (interval 2 s): it has to fire about 5 times in 11 s (a loop of firings was seen in launchservicesd) */
#ifndef WALL
#define WALL 0
#endif
typedef void *dq;
typedef void *ds;
extern const struct dispatch_source_type_s _dispatch_source_type_timer;
extern ds dispatch_source_create(const void *type, unsigned long handle, unsigned long mask, dq q);
extern void dispatch_source_set_timer(ds s, unsigned long long start, unsigned long long interval, unsigned long long leeway);
extern void dispatch_source_set_event_handler(ds s, void (^h)(void));
extern void dispatch_resume(ds s);
extern unsigned long long dispatch_time(unsigned long long when, long long delta);
extern unsigned long long dispatch_walltime(const void *when, long long delta);
extern dq dispatch_get_global_queue(long id, unsigned long flags);
extern unsigned int sleep(unsigned int);
extern unsigned long long mach_absolute_time(void);
extern unsigned long long mach_continuous_time(void);
extern unsigned long long clock_gettime_nsec_np(int);
extern long write(int, const void *, unsigned long);
static volatile int fired;
static volatile unsigned long long first, t0;
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }

int
main(void)
{
	{
		unsigned long long a = mach_absolute_time(), u = clock_gettime_nsec_np(8 /* CLOCK_UPTIME_RAW */), m = clock_gettime_nsec_np(4 /* CLOCK_MONOTONIC_RAW */);
		char b[24]; int k;
		out("timertest: mach_absolute_time - uptime (ms): ");
		{ long long d = (long long)(a - u) / 1000000; int neg = d < 0; if (neg) d = -d; for (k = 22; k >= 0; k--) { b[k] = '0' + d % 10; d /= 10; if (d == 0) break; } b[23] = 0; if (neg) out("-"); out(b + k); out("\n"); }
		(void)m;
		{ long long d = (long long)(mach_continuous_time() - a) / 1000000; int neg = d < 0; if (neg) d = -d; for (k = 22; k >= 0; k--) { b[k] = '0' + d % 10; d /= 10; if (d == 0) break; } b[23] = 0; out("timertest: mach_continuous_time - mach_absolute_time (ms): "); if (neg) out("-"); out(b + k); out("\n"); }
	}
	ds s = dispatch_source_create(&_dispatch_source_type_timer, 0, 0, dispatch_get_global_queue(0, 0));
	int i;

	t0 = mach_absolute_time();
	dispatch_source_set_event_handler(s, ^{ if (!fired) first = mach_absolute_time() - t0; fired++; });
	dispatch_source_set_timer(s, WALL ? dispatch_walltime(0, 2000000000LL) : dispatch_time(0, 2000000000LL), ~0ULL, 100000000ULL);
	dispatch_resume(s);
	for (i = 0; i < 11; i++) sleep(1);
	out(fired == 1 ? "timertest: ok\n" : "timertest: WRONG number of firings\n");
	{ char b[8]; int f = fired > 99 ? 99 : fired; b[0] = '0' + f / 10; b[1] = '0' + f % 10; b[2] = '\n'; b[3] = 0; out("timertest: fired "); out(b); }
	{ char b[24]; int k = 23; unsigned long long v = first / 1000000; b[23] = 0; if (v == 0) b[--k] = '0'; while (v > 0) { b[--k] = '0' + v % 10; v /= 10; } out("timertest: first firing after ms: "); out(b + k); out("\n"); }
	return 0;
}
