/* signals under the real dyld: kill() to oneself, alarm() (setitimer), and sigsuspend() that has to wait for the signal (libdispatch parks the main thread of a daemon in sigsuspend) */
extern int sigsuspend(const unsigned int *mask);
extern unsigned int alarm(unsigned int);
extern void (*signal(int, void (*)(int)))(int);
extern int kill(int, int);
extern int getpid(void);
extern long write(int, const void *, unsigned long);
extern unsigned int sleep(unsigned int);
static volatile int got;
static void h(int s) { got++; }
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }
static void num(const char *p, long v) { char b[24]; int k = 23; b[23] = 0; if (v == 0) b[--k] = '0'; while (v > 0) { b[--k] = '0' + v % 10; v /= 10; } out(p); out(b + k); out("\n"); }

int
main(void)
{
	unsigned int mask = 0;
	long n = 0;

	signal(10, h);					/* SIGUSR1 */
	signal(14, h);					/* SIGALRM */
	kill(getpid(), 10);
	num("sigtest: after kill(self): handler ran ", got);
	got = 0;
	alarm(1);
	sleep(3);
	num("sigtest: alarm(1) then sleep(3): handler ran ", got);
	got = 0;
	alarm(1);
	while (!got && n < 1000000) { sigsuspend(&mask); n++; }
	num("sigtest: alarm(1) then sigsuspend: returns ", n);
	num("sigtest: handler ran ", got);
	return 0;
}
