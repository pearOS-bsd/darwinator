extern unsigned long long dispatch_time(unsigned long long when, long long delta);
extern unsigned long long mach_absolute_time(void);
extern unsigned long long mach_continuous_time(void);
extern long write(int, const void *, unsigned long);
static void out(const char *s) { unsigned long n = 0; while (s[n]) n++; write(1, s, n); }
static void hex(const char *p, unsigned long long v) { char b[20]; int i; for (i = 0; i < 16; i++) b[i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15]; b[16] = '\n'; b[17] = 0; out(p); out(b); }
int main(void)
{
	hex("abs   ", mach_absolute_time());
	hex("cont  ", mach_continuous_time());
	hex("time0 ", dispatch_time(0, 2000000000LL));
	hex("time+ ", dispatch_time(mach_absolute_time(), 2000000000LL));
	return 0;
}
