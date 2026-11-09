extern void hello_print(const char *who);
extern int hello_answer(void);
extern void exit(int);
int main(void) { hello_print("a Mach-O program built on FreeBSD"); exit(hello_answer() == 42 ? 0 : 1); return 0; }
