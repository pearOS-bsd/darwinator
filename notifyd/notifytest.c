/* test client of notifyd: the notify(3) calls of libSystem, as a Mac program makes them */
typedef unsigned int u32;
typedef unsigned long u64;
extern int notify_register_check(const char *name, int *token);
extern int notify_check(int token, int *check);
extern int notify_post(const char *name);
extern int notify_set_state(int token, u64 state);
extern int notify_get_state(int token, u64 *state);
extern int notify_cancel(int token);
extern int shm_open(const char *, int, ...);
extern void *mmap(void *, unsigned long, int, int, int, long);
extern int usleep(unsigned int);
extern long write(int fd, const void *buf, unsigned long n);
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }
static void outx(u64 v) { char b[17]; int i; for (i = 15; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15]; b[16] = 0; out(b); }
static int fails;
static void check(const char *what, int ok, u64 v) { out(ok ? "ok   " : "FAIL "); out(what); out(" = "); outx(v); out("\n"); if (!ok) fails++; }

int
main(void)
{
	int tok = -1, c = -1;
	u64 st = 0;
	u32 r;

	r = notify_register_check("com.pearos.test.one", &tok);
	check("notify_register_check", r == 0 && tok != -1, r);
	r = notify_check(tok, &c); check("first notify_check (a new registration reads as changed or not, both are valid)", r == 0, (u64)c);
	r = notify_check(tok, &c); check("second notify_check sees no change", r == 0 && c == 0, (u64)c);
	{
		int fd = shm_open("apple.shm.notification_center", 0);
		volatile u32 *m = fd >= 0 ? (volatile u32 *)mmap(0, 16384, 1, 1, fd, 0) : 0;

		check("own view of the page opens", fd >= 0 && (long)m != -1, (u64)fd);
		if (fd >= 0 && (long)m != -1) {
			out("page before: "); outx(m[0]); out(" "); outx(m[1]); out(" "); outx(m[2]); out("\n");
			notify_post("com.pearos.test.one");
			usleep(50000);
			out("page after:  "); outx(m[0]); out(" "); outx(m[1]); out(" "); outx(m[2]); out("\n");
		}
	}
	r = notify_post("com.pearos.test.one"); check("notify_post", r == 0, r);
	usleep(50000);			/* a post is a message without an answer: the daemon has to get to it */
	r = notify_check(tok, &c); check("notify_check after the post sees the change", r == 0 && c == 1, (u64)c);
	r = notify_check(tok, &c); check("and then no more", r == 0 && c == 0, (u64)c);
	r = notify_set_state(tok, 0x1234567890abcdefUL); check("notify_set_state", r == 0, r);
	r = notify_get_state(tok, &st); check("notify_get_state gives it back", r == 0 && st == 0x1234567890abcdefUL, st);
	r = notify_cancel(tok); check("notify_cancel", r == 0, r);
	out(fails ? "notifytest: FAILED\n" : "notifytest: all ok\n");
	return (fails != 0);
}
