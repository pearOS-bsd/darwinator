/* the library of the test: hello_print writes a line through libSystem's write (the declarations are written here: no header of an SDK is used) */
extern long write(int fd, const void *buf, unsigned long n);
extern int getpid(void);
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
void hello_print(const char *who) { write(1, "hello from libhello, ", 21); write(1, who, slen(who)); write(1, "\n", 1); }
int hello_answer(void) { return 42; }
