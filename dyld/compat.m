/*
 * libmachcompat.so: GNUstep fixes written in Objective-C (they need @try), loaded by machrun when a program has Objective-C.
 * Each one is a place where Apple's AppKit is more forgiving than GNUstep's.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>

/* The auto layout solver of GNUstep raises "Objective function is unbounded" for some constraint sets that Apple's engine solves; the solver and
 * the views keep what they had and the program goes on (the exception would otherwise end it in Keka's "Critical Error" dialog).  Every way into
 * the solver that can raise is wrapped: GSCassowarySolver resolve / addConstraint: / removeConstraint:, and GSAutoLayoutEngine addConstraint:. */
static IMP o_resolve, o_solver_add, o_solver_remove, o_engine_add;

static void
w_resolve(id self, SEL _cmd)
{
	@try { ((void (*)(id, SEL))o_resolve)(self, _cmd); }
	@catch (NSException *e) { fprintf(stderr, "machrun: layout solver: %s (ignored)\n", [[e reason] UTF8String]); }
}
static void
w_solver_add(id self, SEL _cmd, id c)
{
	@try { ((void (*)(id, SEL, id))o_solver_add)(self, _cmd, c); }
	@catch (NSException *e) { fprintf(stderr, "machrun: layout solver: %s (ignored)\n", [[e reason] UTF8String]); }
}
static void
w_solver_remove(id self, SEL _cmd, id c)
{
	@try { ((void (*)(id, SEL, id))o_solver_remove)(self, _cmd, c); }
	@catch (NSException *e) { fprintf(stderr, "machrun: layout solver: %s (ignored)\n", [[e reason] UTF8String]); }
}
static void
w_engine_add(id self, SEL _cmd, id c)
{
	@try { ((void (*)(id, SEL, id))o_engine_add)(self, _cmd, c); }
	@catch (NSException *e) { fprintf(stderr, "machrun: layout constraint ignored: %s\n", [[e reason] UTF8String]); }
}

static IMP
wrap(const char *cls, SEL sel, IMP w)
{
	Class k = objc_getClass(cls); Method m;
	if (k == Nil || (m = class_getInstanceMethod(k, sel)) == NULL) return (NULL);
	if (method_getTypeEncoding(m)[0] != 'v') return (NULL);		/* only the methods that return nothing */
	return (method_setImplementation(m, w));
}

/* GCD's main queue: a Mac program schedules work on it (dispatch_async(dispatch_get_main_queue(), ...)) and the run loop runs it; GNUstep's run loop does not know
 * libdispatch, so a timer of the main run loop drains the queue. */
#include <dlfcn.h>
@interface MachDispatchPump : NSObject
+ (void)pump:(NSTimer *)t;
@end
@implementation MachDispatchPump
+ (void)pump:(NSTimer *)t
{
	static void (*drain)(void *); static int tried;
	if (!tried) { tried = 1; drain = (void (*)(void *))dlsym(RTLD_DEFAULT, "_dispatch_main_queue_callback_4CF"); }
	if (drain) drain(NULL);
}
@end

__attribute__((constructor)) static void
machcompat_init(void)
{
	{
		NSTimer *t = [NSTimer timerWithTimeInterval:0.02 target:[MachDispatchPump class] selector:@selector(pump:) userInfo:nil repeats:YES];
		[[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
	}
	o_resolve = wrap("GSCassowarySolver", @selector(resolve), (IMP)w_resolve);
	o_solver_add = wrap("GSCassowarySolver", @selector(addConstraint:), (IMP)w_solver_add);
	o_solver_remove = wrap("GSCassowarySolver", @selector(removeConstraint:), (IMP)w_solver_remove);
	o_engine_add = wrap("GSAutoLayoutEngine", @selector(addConstraint:), (IMP)w_engine_add);
}
