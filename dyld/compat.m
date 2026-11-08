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

/* Apple's NSKeyedUnarchiver answers decodeObjectForKey: with the value itself when the archive holds a plain string or number there (Interface Builder's compiled
 * nibs do: the text of NSLocalizableString); GNUstep's raises "value for key is ...". */
static IMP o_decodeObjectForKey;
static id
w_decodeObjectForKey(id self, SEL _cmd, NSString *key)
{
	id map = [self valueForKey:@"_keyMap"];
	id o = map ? [map objectForKey:key] : nil;
	if (o == nil && map) o = [map objectForKey:[@"$" stringByAppendingString:key]];
	if ([o isKindOfClass:[NSString class]] || [o isKindOfClass:[NSNumber class]]) return o;
	return ((id (*)(id, SEL, NSString *))o_decodeObjectForKey)(self, _cmd, key);
}

/* NSFormatter's abstract methods (getObjectValue:forString:errorDescription:, stringForObjectValue:, ...) raise "should be overridden by subclass"; a subclass compiled on the Mac
 * defines them with Apple's type encoding and GNUstep's call sites still reach the base method.  The base methods look for the receiver class's own implementation first. */
static IMP o_fmt_getObjectValue;
static BOOL
w_fmt_getObjectValue(id self, SEL _cmd, id *obj, NSString *string, NSString **err)
{
	Method m = class_getInstanceMethod(object_getClass(self), sel_registerName("getObjectValue:forString:errorDescription:"));	/* an untyped selector: _cmd carries the types of the call site */
	IMP imp = m ? method_getImplementation(m) : NULL;
	if (imp != NULL && imp != (IMP)w_fmt_getObjectValue && imp != o_fmt_getObjectValue)
		return ((BOOL (*)(id, SEL, id *, NSString *, NSString **))imp)(self, _cmd, obj, string, err);
	return ((BOOL (*)(id, SEL, id *, NSString *, NSString **))o_fmt_getObjectValue)(self, _cmd, obj, string, err);
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
	{ static int n; if (getenv("MACHRUN_DEBUG") && n++ < 3) fprintf(stderr, "machrun: GCD main queue pump %p\n", (void *)drain); }
	if (drain) drain(NULL);
}
@end

__attribute__((constructor)) static void
machcompat_init(void)
{
	{
		NSTimer *t = [NSTimer timerWithTimeInterval:0.02 target:[MachDispatchPump class] selector:@selector(pump:) userInfo:nil repeats:YES];
		NSRunLoop *rl = [NSRunLoop mainRunLoop];
		[rl addTimer:t forMode:NSDefaultRunLoopMode];
		[rl addTimer:t forMode:@"NSEventTrackingRunLoopMode"];
		[rl addTimer:t forMode:@"NSModalPanelRunLoopMode"];
	}
	{
		Class k = objc_getClass("NSKeyedUnarchiver"); Method m;
		if (k != Nil && (m = class_getInstanceMethod(k, @selector(decodeObjectForKey:))) != NULL) o_decodeObjectForKey = method_setImplementation(m, (IMP)w_decodeObjectForKey);
	}
	{
		Class k = objc_getClass("NSFormatter"); Method m;
		if (k != Nil && (m = class_getInstanceMethod(k, @selector(getObjectValue:forString:errorDescription:))) != NULL) o_fmt_getObjectValue = method_setImplementation(m, (IMP)w_fmt_getObjectValue);
	}
	o_resolve = wrap("GSCassowarySolver", @selector(resolve), (IMP)w_resolve);
	o_solver_add = wrap("GSCassowarySolver", @selector(addConstraint:), (IMP)w_solver_add);
	o_solver_remove = wrap("GSCassowarySolver", @selector(removeConstraint:), (IMP)w_solver_remove);
	o_engine_add = wrap("GSAutoLayoutEngine", @selector(addConstraint:), (IMP)w_engine_add);
}

/* ---- CoreFoundation string encodings: an Apple program that is written in Objective-C treats a CFStringRef as an NSString (toll-free bridging);
 * these functions give it that, on top of NSString.  machrun looks for them here when its own (C) CoreFoundation subset has no such function. ---- */
static const struct { uint32_t cf; NSStringEncoding ns; const char *iana; } cfenc[] = {
	{ 0x0600, NSASCIIStringEncoding, "us-ascii" }, { 0x08000100, NSUTF8StringEncoding, "utf-8" }, { 0x0201, NSISOLatin1StringEncoding, "iso-8859-1" },
	{ 0x0500, NSWindowsCP1252StringEncoding, "windows-1252" }, { 0, NSMacOSRomanStringEncoding, "macintosh" }, { 0x0100, NSUTF16StringEncoding, "utf-16" },
	{ 0x10000100, NSUTF16BigEndianStringEncoding, "utf-16be" }, { 0x14000100, NSUTF16LittleEndianStringEncoding, "utf-16le" },
	{ 0x0c000100, NSUTF32StringEncoding, "utf-32" }, { 0x18000100, NSUTF32BigEndianStringEncoding, "utf-32be" }, { 0x1c000100, NSUTF32LittleEndianStringEncoding, "utf-32le" },
	{ 0x0202, NSISOLatin2StringEncoding, "iso-8859-2" }, { 0x0501, NSWindowsCP1250StringEncoding, "windows-1250" }, { 0x0502, NSWindowsCP1251StringEncoding, "windows-1251" },
	{ 0x0503, NSWindowsCP1253StringEncoding, "windows-1253" }, { 0x0504, NSWindowsCP1254StringEncoding, "windows-1254" }, { 0x0421, NSShiftJISStringEncoding, "shift_jis" },
	{ 0x0A01, NSJapaneseEUCStringEncoding, "euc-jp" },
};
#define	NENC	(sizeof(cfenc) / sizeof(cfenc[0]))
#define	CFINVALID	0xffffffffU

static NSStringEncoding
to_ns(uint32_t cf)
{
	size_t i;
	for (i = 0; i < NENC; i++) if (cfenc[i].cf == cf) return (cfenc[i].ns);
	return (0);
}
uint32_t CFStringConvertEncodingToNSStringEncoding(uint32_t cf) { NSStringEncoding e = to_ns(cf); return (e ? (uint32_t)e : (cf | 0x80000000U)); }
uint32_t
CFStringConvertNSStringEncodingToEncoding(uint32_t ns)
{
	size_t i;
	for (i = 0; i < NENC; i++) if (cfenc[i].ns == ns) return (cfenc[i].cf);
	return ((ns & 0x80000000U) ? (ns & ~0x80000000U) : CFINVALID);
}
uint32_t
CFStringConvertIANACharSetNameToEncoding(id name)
{
	size_t i; NSString *l; l = [[(NSString *)name description] lowercaseString];
	for (i = 0; i < NENC; i++) if ([l isEqualToString:[NSString stringWithUTF8String:cfenc[i].iana]]) return (cfenc[i].cf);
	if ([l isEqualToString:@"ascii"] || [l isEqualToString:@"ansi_x3.4-1968"]) return (0x0600);
	if ([l isEqualToString:@"utf8"]) return (0x08000100);
	if ([l isEqualToString:@"latin1"] || [l isEqualToString:@"iso8859-1"]) return (0x0201);
	return (CFINVALID);
}
id
CFStringConvertEncodingToIANACharSetName(uint32_t cf)
{
	size_t i;
	for (i = 0; i < NENC; i++) if (cfenc[i].cf == cf) return ([NSString stringWithUTF8String:cfenc[i].iana]);
	return (nil);
}
static const uint32_t availenc[] = { 0x08000100, 0x0100, 0x10000100, 0x14000100, 0x0600, 0x0201, 0x0500, 0, 0x0202, 0x0501, 0x0502, 0x0503, 0x0504, 0x0421, 0x0A01, CFINVALID };
const uint32_t *CFStringGetListOfAvailableEncodings(void) { return (availenc); }

id
CFStringCreateWithBytes(void *alloc, const uint8_t *bytes, long n, uint32_t enc, unsigned char isExternal)
{
	NSStringEncoding ne = to_ns(enc); id s;
	(void)alloc; (void)isExternal;
	if (ne == 0) return (nil);
	if (ne == NSUTF16StringEncoding && n >= 2) {		/* Apple's UTF-16 means "BOM or host order" */
		if (bytes[0] == 0xFE && bytes[1] == 0xFF) ne = NSUTF16BigEndianStringEncoding;
	}
	s = [[NSString alloc] initWithBytes:bytes length:(NSUInteger)n encoding:ne];
	return (s);		/* a +1 reference, as CFStringCreate... gives; the program releases it */
}
const char *CFStringGetCStringPtr(id s, uint32_t enc) { (void)s; (void)enc; return (NULL); }	/* "not available as such": callers then copy */
long
CFStringGetBytes(id s, long loc, long len, uint32_t enc, uint8_t lossByte, unsigned char isExternal, uint8_t *buf, long max, long *used)
{
	NSStringEncoding ne = to_ns(enc); NSString *sub; NSData *d; long n;
	(void)lossByte; (void)isExternal;
	if (ne == 0) return (0);
	sub = [(NSString *)s substringWithRange:NSMakeRange(loc, len)];
	d = [sub dataUsingEncoding:ne allowLossyConversion:NO];
	if (d == nil) return (0);
	n = (long)[d length];
	if (buf != NULL) { if (n > max) return (0); memcpy(buf, [d bytes], n); }
	if (used) *used = n;
	return (len);
}
