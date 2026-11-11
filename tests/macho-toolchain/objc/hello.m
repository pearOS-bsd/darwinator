/* an Objective-C program for a Mac runtime, built on FreeBSD with clang and ld64.lld, no header of Apple's: a root class, a method, a message send through libobjc */
typedef struct objc_class *Class;
typedef struct objc_object { Class isa; } *id;
typedef struct objc_selector *SEL;
extern id objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern id objc_msgSend(id, SEL, ...);
extern long write(int, const void *, unsigned long);
extern void exit(int);

__attribute__((objc_root_class))
@interface Hello
+ (id)class;
+ (int)answer;
@end
@implementation Hello
+ (id)class { return (id)self; }
+ (int)answer { return 42; }
@end

int
main(void)
{
	id c = objc_getClass("Hello");
	int (*send)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
	int r = send(c, sel_registerName("answer"));

	write(1, r == 42 ? "objc: Hello answer = 42\n" : "objc: wrong answer\n", r == 42 ? 24 : 20);
	exit(r == 42 ? 0 : 1);
}
