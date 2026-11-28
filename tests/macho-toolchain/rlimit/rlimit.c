/* rlimit: getrlimit(RLIMIT_NOFILE), sysconf(_SC_STREAM_MAX) and fopen of a Mac program (the libc of the Mac: its getrlimit is the UNIX03
 * variant, which adds _RLIMIT_POSIX_FLAG 0x1000 to the resource; darwinator refused that, every fopen() then failed with EMFILE).
 * Exit 0 and "rlimit ok" when all three work. */
struct rlimit { unsigned long long cur, max; };
extern int getrlimit(int, struct rlimit *);
extern long sysconf(int);
typedef struct FILE FILE;
extern FILE *fopen(const char *, const char *);
extern int fclose(FILE *);
extern int fputs(const char *, FILE *);
extern int unlink(const char *);
extern int printf(const char *, ...);
extern int *__error(void);
int main(void)
{
	struct rlimit r = { 0, 0 };
	int e = getrlimit(8 /* RLIMIT_NOFILE */, &r);
	long smax = sysconf(26 /* _SC_STREAM_MAX on Darwin */);
	FILE *f = fopen("/tmp/darwinator-rlimit-test", "w");
	int ferr = f ? 0 : *__error();
	if (f) { fputs("ok\n", f); fclose(f); unlink("/tmp/darwinator-rlimit-test"); }
	printf("getrlimit %d (cur %llu), _SC_STREAM_MAX %ld, fopen %s (errno %d)\n", e, r.cur, smax, f ? "ok" : "FAILED", ferr);
	if (e == 0 && r.cur > 0 && smax > 0 && f) { printf("rlimit ok\n"); return 0; }
	return 1;
}
