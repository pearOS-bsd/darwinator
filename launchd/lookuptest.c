/* test client: asks the bootstrap port for a service by name, as a Mac program does (bootstrap_look_up), and prints the result.  sh: ./lookuptest com.apple.bsd.dirhelper */
typedef unsigned int u32;
extern u32 bootstrap_port;
extern int bootstrap_look_up(u32 bp, const char *name, u32 *port);
extern int kill(int pid, int sig);
extern int getpid(void);
extern long write(int fd, const void *buf, unsigned long n);
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }
static void outx(u32 v) { char b[9]; int i; for (i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15]; b[8] = 0; out(b); }
int
main(int argc, char **argv)
{
	u32 port = 0;
	int kr;

	if (argc > 2)
		kill(getpid(), 17);		/* SIGSTOP: a debugger attaches here */
	kr = bootstrap_look_up(bootstrap_port, argc > 1 ? argv[1] : "com.apple.bsd.dirhelper", &port);

	out("bootstrap_look_up: kr "); outx((u32)kr); out(" port "); outx(port); out("\n");
	return (kr != 0);
}
