/*
 * libmachcompat.so: GNUstep fixes written in Objective-C (they need @try), loaded by machrun when a program has Objective-C.
 * Each one is a place where Apple's AppKit is more forgiving than GNUstep's.
 */
#import <Foundation/Foundation.h>
typedef const void *CFTypeRef; typedef const void *CFStringRef;
#include <objc/runtime.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>

/* The auto layout solver of GNUstep raises "Objective function is unbounded" for some constraint sets that Apple's engine solves; the solver and
 * the views keep what they had and the program goes on (the exception would otherwise end it in Keka's "Critical Error" dialog).  Every way into
 * the solver that can raise is wrapped: GSCassowarySolver resolve / addConstraint: / removeConstraint:, and GSAutoLayoutEngine addConstraint:. */
/* An image without a size (a PDF or an asset that GNUstep cannot draw) cannot be drawn into; Apple's NSImage draws nothing and goes on. */
static IMP o_lockFocusRep;
static void
w_lockFocusRep(id self, SEL _cmd, id rep)
{
	NSSize sz = ((NSSize (*)(id, SEL))class_getMethodImplementation(object_getClass(self), @selector(size)))(self, @selector(size));
	if (sz.width <= 0 || sz.height <= 0) ((void (*)(id, SEL, NSSize))class_getMethodImplementation(object_getClass(self), @selector(setSize:)))(self, @selector(setSize:), NSMakeSize(1, 1));
	((void (*)(id, SEL, id))o_lockFocusRep)(self, _cmd, rep);
}
static IMP o_lockFocus;
#define	objc_msgSend_stret_compat	class_getMethodImplementation(object_getClass(self), @selector(size))
static void
w_lockFocus(id self, SEL _cmd)
{
	NSSize sz = ((NSSize (*)(id, SEL))objc_msgSend_stret_compat)(self, @selector(size));
	if (sz.width <= 0 || sz.height <= 0) ((void (*)(id, SEL, NSSize))class_getMethodImplementation(object_getClass(self), @selector(setSize:)))(self, @selector(setSize:), NSMakeSize(1, 1));
	((void (*)(id, SEL))o_lockFocus)(self, _cmd);
}
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
	/* what the main queue did (a view that was added or a mode that changed) is shown: a window that needs display gets it now, not at the next event */
	{ static Class ak; static id (*shared)(id, SEL); id app, wins;
	  if (ak == Nil) ak = NSClassFromString(@"NSApplication");
	  if (ak != Nil && (app = ((id (*)(id, SEL))class_getMethodImplementation(object_getClass(ak), @selector(sharedApplication)))(ak, @selector(sharedApplication))) != nil) {
		wins = ((id (*)(id, SEL))class_getMethodImplementation(object_getClass(app), @selector(windows)))(app, @selector(windows));
		[wins makeObjectsPerformSelector:@selector(displayIfNeeded)];
	  }
	  (void)shared; }
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
	o_lockFocusRep = wrap("NSImage", @selector(lockFocusOnRepresentation:), (IMP)w_lockFocusRep);
	o_lockFocus = wrap("NSImage", @selector(lockFocus), (IMP)w_lockFocus);
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

/* ---- constant literals of the compiler (clang -fobjc-constant-literals, Xcode 16 and later): @[...], @{...} and @123 of constants are made at build time as structures in
 * __DATA,__objc_arrayobj / __objc_dictobj / __objc_intobj whose isa is bound to these classes.  Each reads its structure in place; they are never released. ---- */
struct carray { Class isa; NSUInteger count; id *objects; };
struct cdict { Class isa; NSUInteger options, count; id *keys, *objects; };
struct cint { Class isa; const char *type; long long value; };

@interface NSConstantArray : NSArray @end
@implementation NSConstantArray
- (NSUInteger)count { return ((struct carray *)self)->count; }
- (id)objectAtIndex:(NSUInteger)i
{
	struct carray *a = (struct carray *)self;
	if (i >= a->count) [NSException raise:NSRangeException format:@"index %lu beyond bounds %lu", (unsigned long)i, (unsigned long)a->count];
	return a->objects[i];
}
- (id)retain { return self; }
- (oneway void)release { }
- (id)autorelease { return self; }
- (NSUInteger)retainCount { return NSUIntegerMax; }
- (void)dealloc { }
- (id)copyWithZone:(NSZone *)z { return self; }
@end

@interface NSConstantDictionary : NSDictionary @end
@implementation NSConstantDictionary
- (NSUInteger)count { return ((struct cdict *)self)->count; }
- (id)objectForKey:(id)key
{
	struct cdict *d = (struct cdict *)self; NSUInteger i;
	if (key == nil) return nil;
	for (i = 0; i < d->count; i++) if (d->keys[i] == key || [d->keys[i] isEqual:key]) return d->objects[i];
	return nil;
}
- (NSEnumerator *)keyEnumerator
{
	struct cdict *d = (struct cdict *)self;
	return [[NSArray arrayWithObjects:d->keys count:d->count] objectEnumerator];
}
- (id)retain { return self; }
- (oneway void)release { }
- (id)autorelease { return self; }
- (NSUInteger)retainCount { return NSUIntegerMax; }
- (void)dealloc { }
- (id)copyWithZone:(NSZone *)z { return self; }
@end

@interface NSConstantIntegerNumber : NSNumber @end
@implementation NSConstantIntegerNumber
#define	V	(((struct cint *)self)->value)
- (const char *)objCType { return ((struct cint *)self)->type; }
- (void)getValue:(void *)p { *(long long *)p = V; }
- (char)charValue { return (char)V; }
- (unsigned char)unsignedCharValue { return (unsigned char)V; }
- (short)shortValue { return (short)V; }
- (unsigned short)unsignedShortValue { return (unsigned short)V; }
- (int)intValue { return (int)V; }
- (unsigned int)unsignedIntValue { return (unsigned int)V; }
- (long)longValue { return (long)V; }
- (unsigned long)unsignedLongValue { return (unsigned long)V; }
- (long long)longLongValue { return V; }
- (unsigned long long)unsignedLongLongValue { return (unsigned long long)V; }
- (NSInteger)integerValue { return (NSInteger)V; }
- (NSUInteger)unsignedIntegerValue { return (NSUInteger)V; }
- (float)floatValue { return (float)V; }
- (double)doubleValue { return (double)V; }
- (BOOL)boolValue { return V != 0; }
- (NSString *)stringValue { return [NSString stringWithFormat:@"%lld", V]; }
- (NSString *)description { return [self stringValue]; }
- (NSString *)descriptionWithLocale:(id)l { return [self stringValue]; }
- (NSUInteger)hash { return (NSUInteger)V; }
- (BOOL)isEqualToNumber:(NSNumber *)n { return n != nil && [n doubleValue] == (double)V && [n longLongValue] == V; }
- (BOOL)isEqual:(id)o { return [o isKindOfClass:[NSNumber class]] && [self isEqualToNumber:o]; }
- (NSComparisonResult)compare:(NSNumber *)n
{
	double a = (double)V, b = [n doubleValue];
	return a < b ? NSOrderedAscending : a > b ? NSOrderedDescending : NSOrderedSame;
}
- (id)retain { return self; }
- (oneway void)release { }
- (id)autorelease { return self; }
- (NSUInteger)retainCount { return NSUIntegerMax; }
- (void)dealloc { }
- (id)copyWithZone:(NSZone *)z { return self; }
@end

/* ---- compiled nibs in the format "NIBArchive" (Xcode 16 / macOS 15): GNUstep reads the NSKeyedArchiver nibs of older Xcode.  The same object graph is in both; this makes the property list
 * of the old format from the new file (tools/nibarchive.py does the same for a file), when a keyed unarchiver is made on such data. ---- */
static uint64_t
nib_varint(const uint8_t *d, size_t *p)
{
	uint64_t r = 0; int s = 0; uint8_t b;
	do { b = d[(*p)++]; r |= (uint64_t)(b & 0x7f) << s; s += 7; } while (!(b & 0x80));
	return (r);
}
static id nib_uid(NSUInteger i) { return [NSDictionary dictionaryWithObject:[NSNumber numberWithUnsignedLong:i] forKey:@"CF$UID"]; }

NSData *
machrun_nibarchive_to_keyed(NSData *data)
{
	const uint8_t *d = [data bytes]; size_t len = [data length], p;
	uint32_t h[10]; uint32_t i, j;
	NSMutableArray *keys = [NSMutableArray array], *classes = [NSMutableArray array];
	if (len < 50 || memcmp(d, "NIBArchive", 10) != 0) return nil;
	memcpy(h, d + 10, sizeof(h));		/* version1, version2, nobj, objoff, nkey, keyoff, nval, valoff, ncls, clsoff (little endian) */
	p = h[5];
	for (i = 0; i < h[4]; i++) { size_t n = nib_varint(d, &p); [keys addObject:[[[NSString alloc] initWithBytes:d + p length:n encoding:NSUTF8StringEncoding] autorelease]]; p += n; }
	p = h[9];
	for (i = 0; i < h[8]; i++) {
		size_t n = nib_varint(d, &p), ne = nib_varint(d, &p); p += 4 * ne;
		if (n && d[p + n - 1] == 0) n--;
		[classes addObject:[[[NSString alloc] initWithBytes:d + p length:n encoding:NSUTF8StringEncoding] autorelease]]; p += (n + (n < 1 ? 0 : 0));
		if (d[p] == 0) p++;
	}
	/* values: key index, type, payload */
	NSMutableArray *vkey = [NSMutableArray array], *vval = [NSMutableArray array];
	p = h[7];
	for (i = 0; i < h[6]; i++) {
		uint64_t k = nib_varint(d, &p); uint8_t t = d[p++]; id v = [NSNull null];
		switch (t) {
		case 0: v = [NSNumber numberWithInt:(int8_t)d[p]]; p += 1; break;
		case 1: { int16_t x; memcpy(&x, d + p, 2); v = [NSNumber numberWithInt:x]; p += 2; break; }
		case 2: { int32_t x; memcpy(&x, d + p, 4); v = [NSNumber numberWithInt:x]; p += 4; break; }
		case 3: { int64_t x; memcpy(&x, d + p, 8); v = [NSNumber numberWithLongLong:x]; p += 8; break; }
		case 4: v = [NSNumber numberWithBool:YES]; break;
		case 5: v = [NSNumber numberWithBool:NO]; break;
		case 6: { float x; memcpy(&x, d + p, 4); v = [NSNumber numberWithDouble:x]; p += 4; break; }
		case 7: { double x; memcpy(&x, d + p, 8); v = [NSNumber numberWithDouble:x]; p += 8; break; }
		case 8: { size_t n = nib_varint(d, &p); v = [NSData dataWithBytes:d + p length:n]; p += n; break; }
		case 9: break;
		case 10: { uint32_t x; memcpy(&x, d + p, 4); v = nib_uid(x); p += 4; break; }
		default: return nil;
		}
		[vkey addObject:k < [keys count] ? [keys objectAtIndex:k] : @""]; [vval addObject:v];
	}
	/* objects */
	NSUInteger nobj = h[2]; NSMutableArray *out = [NSMutableArray arrayWithCapacity:nobj + [classes count] + 1];
	NSMutableDictionary *top = [NSMutableDictionary dictionary];
	[out addObject:@"$null"];
	p = h[3];
	for (i = 0; i < nobj; i++) {
		uint64_t c = nib_varint(d, &p), s = nib_varint(d, &p), n = nib_varint(d, &p);
		NSString *cn = c < [classes count] ? [classes objectAtIndex:c] : @"NSObject"; id o;
		if (i == 0) { for (j = 0; j < n; j++) [top setObject:[vval objectAtIndex:s + j] forKey:[vkey objectAtIndex:s + j]]; continue; }
		if ([cn isEqualToString:@"NSString"]) {
			NSData *b = nil; for (j = 0; j < n; j++) if ([[vkey objectAtIndex:s + j] isEqualToString:@"NS.bytes"]) b = [vval objectAtIndex:s + j];
			o = b ? [[[NSString alloc] initWithData:b encoding:NSUTF8StringEncoding] autorelease] : @"";
			if (o == nil) o = @"";
		} else if ([cn isEqualToString:@"NSNumber"]) {
			o = [NSNumber numberWithInt:0];
			for (j = 0; j < n; j++) { NSString *k = [vkey objectAtIndex:s + j]; if ([k hasPrefix:@"NS."]) o = [vval objectAtIndex:s + j]; }
		} else {
			NSMutableDictionary *dict = [NSMutableDictionary dictionary]; NSMutableArray *objs = [NSMutableArray array], *ks = [NSMutableArray array];
			BOOL coll = [cn isEqualToString:@"NSArray"] || [cn isEqualToString:@"NSMutableArray"] || [cn isEqualToString:@"NSSet"] || [cn isEqualToString:@"NSMutableSet"];
			BOOL dic = [cn isEqualToString:@"NSDictionary"] || [cn isEqualToString:@"NSMutableDictionary"];
			for (j = 0; j < n; j++) {
				NSString *k = [vkey objectAtIndex:s + j]; id v = [vval objectAtIndex:s + j];
				if ([v isKindOfClass:[NSNull class]]) continue;
				if ([k isEqualToString:@"UINibEncoderEmptyKey"]) { [objs addObject:v]; continue; }
				if ((coll || dic) && [k isEqualToString:@"NSInlinedValue"]) continue;
				if ([cn isEqualToString:@"NSData"] || [cn isEqualToString:@"NSMutableData"]) if ([k isEqualToString:@"NS.bytes"]) k = @"NS.data";
				if ([cn isEqualToString:@"NSMutableString"] && [k isEqualToString:@"NS.bytes"]) { v = [[[NSString alloc] initWithData:v encoding:NSUTF8StringEncoding] autorelease]; k = @"NS.string"; }
				[dict setObject:v forKey:k];
			}
			if (dic) { NSUInteger q; for (q = 0; q + 1 < [objs count]; q += 2) { [ks addObject:[objs objectAtIndex:q]]; } NSMutableArray *vs = [NSMutableArray array]; for (q = 1; q < [objs count]; q += 2) [vs addObject:[objs objectAtIndex:q]]; [dict setObject:ks forKey:@"NS.keys"]; [dict setObject:vs forKey:@"NS.objects"]; }
			else if (coll) [dict setObject:objs forKey:@"NS.objects"];
			[dict setObject:nib_uid(nobj + c) forKey:@"$class"];
			o = dict;
		}
		[out addObject:o];
	}
	for (i = 0; i < [classes count]; i++) {
		NSString *cn = [classes objectAtIndex:i];
		[out addObject:[NSDictionary dictionaryWithObjectsAndKeys:cn, @"$classname", [NSArray arrayWithObjects:cn, @"NSObject", nil], @"$classes", nil]];
	}
	NSDictionary *plist = [NSDictionary dictionaryWithObjectsAndKeys:@"NSKeyedArchiver", @"$archiver", [NSNumber numberWithInt:100000], @"$version", top, @"$top", out, @"$objects", nil];
	return [NSPropertyListSerialization dataFromPropertyList:plist format:NSPropertyListBinaryFormat_v1_0 errorDescription:NULL];
}

static IMP o_kuInit;
static id
w_kuInit(id self, SEL _cmd, NSData *data)
{
	if (getenv("MACHRUN_DEBUG")) fprintf(stderr, "machrun: nib: initForReadingWithData: %s %lu bytes\n", class_getName(object_getClass(self)), (unsigned long)[data length]);
	if ([data isKindOfClass:[NSData class]] && [data length] > 10 && memcmp([data bytes], "NIBArchive", 10) == 0) {
		NSData *conv = machrun_nibarchive_to_keyed(data);
		if (conv != nil) { if (getenv("MACHRUN_DEBUG")) fprintf(stderr, "machrun: nib: NIBArchive converted (%lu -> %lu bytes)\n", (unsigned long)[data length], (unsigned long)[conv length]); data = conv; }
	}
	return ((id (*)(id, SEL, id))o_kuInit)(self, _cmd, data);
}
__attribute__((constructor)) static void
nibarchive_init(void)
{
	(void)[NSKeyedUnarchiver class]; (void)[NSClassFromString(@"GSKeyedUnarchiver") class];
	{ Method m = class_getInstanceMethod(objc_getClass("NSKeyedUnarchiver"), @selector(initForReadingWithData:)); if (m) o_kuInit = method_setImplementation(m, (IMP)w_kuInit); }
	if (getenv("MACHRUN_DEBUG") && getenv("MACHRUN_DEBUG_NIB")) {
		const char *names[] = { "NSKeyedUnarchiver", "GSKeyedUnarchiver", "GSNibKeyedUnarchiver", "GSXibKeyedUnarchiver" }; int q;
		for (q = 0; q < 4; q++) { Class k = objc_getClass(names[q]); fprintf(stderr, "machrun: nib: %s class %p initForReadingWithData: %p decodeObjectForKey: %p\n", names[q], (void *)k, k ? (void *)class_getInstanceMethod(k, @selector(initForReadingWithData:)) : NULL, k ? (void *)class_getInstanceMethod(k, @selector(decodeObjectForKey:)) : NULL); }
		fprintf(stderr, "machrun: nib: initForReadingWithData: wrapped %p\n", (void *)o_kuInit);
	}
}

/* Sparkle (the update framework that Mac applications carry; its updater is an object of the main nib): there are no updates for programs that run here, so the updater is an object that does nothing.
 * Made only when nothing made a class of that name before (machrun makes an empty placeholder for a program that binds to the class). */
static id su_self(id self, SEL _cmd) { return self; }
static id su_initWithCoder(id self, SEL _cmd, id coder) { return self; }
static void su_void(id self, SEL _cmd) { }
static void su_void1(id self, SEL _cmd, id a) { }
static BOOL su_no(id self, SEL _cmd) { return NO; }
static id su_nil(id self, SEL _cmd) { return nil; }
static id su_shared(id self, SEL _cmd) { static id u; if (u == nil) u = [[self alloc] init]; return u; }
__attribute__((constructor)) static void
sparkle_init(void)
{
	Class k;
	if (objc_getClass("SUUpdater") != Nil) return;
	if ((k = objc_allocateClassPair(NSClassFromString(@"NSObject"), "SUUpdater", 0)) == Nil) return;
	class_addMethod(k, @selector(initWithCoder:), (IMP)su_initWithCoder, "@24@0:8@16");
	class_addMethod(k, @selector(encodeWithCoder:), (IMP)su_void1, "v24@0:8@16");
	class_addMethod(k, @selector(checkForUpdates:), (IMP)su_void1, "v24@0:8@16");
	class_addMethod(k, @selector(checkForUpdatesInBackground), (IMP)su_void, "v16@0:8");
	class_addMethod(k, @selector(automaticallyChecksForUpdates), (IMP)su_no, "c16@0:8");
	class_addMethod(k, @selector(setAutomaticallyChecksForUpdates:), (IMP)su_void1, "v20@0:8c16");
	class_addMethod(k, @selector(setDelegate:), (IMP)su_void1, "v24@0:8@16");
	class_addMethod(k, @selector(delegate), (IMP)su_nil, "@16@0:8");
	class_addMethod(k, @selector(updateInProgress), (IMP)su_no, "c16@0:8");
	objc_registerClassPair(k);
	class_addMethod(object_getClass(k), @selector(sharedUpdater), (IMP)su_shared, "@16@0:8");
	(void)su_self;
}

/* Images that GNUstep cannot give (SF Symbols of macOS 11, and the images of an Assets.car asset catalog): an empty image of the usual size.  Code that builds a toolbar or a menu out of them
 * (arrays with the image in them) goes on instead of ending on a nil; nothing is drawn where such an image goes. */
static id
empty_image(void)
{
	Class k = NSClassFromString(@"NSImage"); id img;
	if (k == Nil) return nil;
	img = [k alloc];
	img = ((id (*)(id, SEL, NSSize))class_getMethodImplementation(k, @selector(initWithSize:)))(img, @selector(initWithSize:), NSMakeSize(18, 18));
	return [img autorelease];
}
static id
symbol_image(id self, SEL _cmd, NSString *name, NSString *desc) { return empty_image(); }
static IMP o_imageNamed;
static id
w_imageNamed(id self, SEL _cmd, NSString *name)
{
	id img = ((id (*)(id, SEL, id))o_imageNamed)(self, _cmd, name);
	return img != nil ? img : empty_image();
}
__attribute__((constructor)) static void
images_init(void)
{
	if (getenv("MACHRUN_NOIMAGES")) return;
	Class k = NSClassFromString(@"NSImage"); Method m;
	if (k == Nil) return;
	(void)[k class];
	class_addMethod(object_getClass(k), @selector(imageWithSystemSymbolName:accessibilityDescription:), (IMP)symbol_image, "@32@0:8@16@24");
	class_addMethod(object_getClass(k), @selector(imageWithSystemSymbolName:variableValue:accessibilityDescription:), (IMP)symbol_image, "@40@0:8@16d24@32");
	if (getenv("MACHRUN_NOEMPTYIMAGES") == NULL && (m = class_getClassMethod(k, @selector(imageNamed:))) != NULL) o_imageNamed = method_setImplementation(m, (IMP)w_imageNamed);
}

/* -1 means "nothing" to the selection setters of AppKit (AppCleaner deselects its segmented control that way); GNUstep raises an exception for the index. */
static IMP o_selSeg, o_selTab;
static void w_selSeg(id self, SEL _cmd, NSInteger i) { if (i >= 0) ((void (*)(id, SEL, NSInteger))o_selSeg)(self, _cmd, i); }
static void w_selTab(id self, SEL _cmd, NSInteger i) { if (i >= 0) ((void (*)(id, SEL, NSInteger))o_selTab)(self, _cmd, i); }
__attribute__((constructor)) static void
selection_init(void)
{
	if (getenv("MACHRUN_NOSEL")) return;
	Class k; Method m;
	if ((k = NSClassFromString(@"NSSegmentedControl")) != Nil) { (void)[k class]; if ((m = class_getInstanceMethod(k, @selector(setSelectedSegment:))) != NULL) o_selSeg = method_setImplementation(m, (IMP)w_selSeg); }
	if ((k = NSClassFromString(@"NSTabView")) != Nil) { (void)[k class]; if ((m = class_getInstanceMethod(k, @selector(selectTabViewItemAtIndex:))) != NULL) o_selTab = method_setImplementation(m, (IMP)w_selTab); }
}

/* NSAnimationContext: GNUstep has no block based animations; the changes are made at once and the completion handler runs at once, as for an animation of zero duration (a program that
 * shows its next view in the handler would wait for ever). */
typedef struct { void *isa; int flags, reserved; void (*invoke)(void *, ...); void *desc; } MBlock;
static id
anim_current(id self, SEL _cmd)
{
	Class k = NSClassFromString(@"NSObject");
	static id ctx; if (ctx == nil) ctx = [[k alloc] init];
	return ctx;
}
static void
anim_run(id self, SEL _cmd, void *changes, void *completion)
{
	if (changes) ((void (*)(void *, id))((MBlock *)changes)->invoke)(changes, anim_current(self, _cmd));
	if (completion) ((void (*)(void *))((MBlock *)completion)->invoke)(completion);
}
static void
anim_begin(id self, SEL _cmd) { }
static void
anim_setduration(id self, SEL _cmd, double d) { }
static void
anim_setbool(id self, SEL _cmd, BOOL b) { }
static void
anim_perform(id self, SEL _cmd, void *changes)
{
	if (changes) ((void (*)(void *, id))((MBlock *)changes)->invoke)(changes, anim_current(self, _cmd));
}
__attribute__((constructor)) static void
animation_init(void)
{
	if (getenv("MACHRUN_NOANIM")) return;
	Class k = NSClassFromString(@"NSAnimationContext"); Class m;
	if (k == Nil) return;
	(void)[k class]; m = object_getClass(k);
#define	SETCLS(sel, imp, enc) do { Method mm = class_getClassMethod(k, sel); if (mm != NULL) method_setImplementation(mm, (IMP)imp); else class_addMethod(m, sel, (IMP)imp, enc); } while (0)
	SETCLS(@selector(runAnimationGroup:completionHandler:), anim_run, "v32@0:8@?16@?24");
	SETCLS(@selector(runAnimationGroup:), anim_perform, "v24@0:8@?16");
	SETCLS(@selector(beginGrouping), anim_begin, "v16@0:8");
	SETCLS(@selector(endGrouping), anim_begin, "v16@0:8");
	SETCLS(@selector(currentContext), anim_current, "@16@0:8");
	if (getenv("MACHRUN_DEBUG")) fprintf(stderr, "machrun: NSAnimationContext methods set\n");
	{ Method im = class_getInstanceMethod(k, @selector(setDuration:)); if (im) method_setImplementation(im, (IMP)anim_setduration); else class_addMethod(k, @selector(setDuration:), (IMP)anim_setduration, "v24@0:8d16"); }
	class_addMethod(k, @selector(setAllowsImplicitAnimation:), (IMP)anim_setbool, "v20@0:8c16");
}

/* ---- UTType (the type system of macOS 11, which a program of macOS 15 uses everywhere): identifiers and the conformance between them, for the types that programs ask about; a URL gets its
 * type from what it is on the disk (NSURLContentTypeKey) ---- */
static const struct { const char *id, *parents; const char *ext; } uttab[] = {
	{ "public.item", "", "" }, { "public.data", "public.item", "" }, { "public.content", "public.item", "" }, { "public.directory", "public.item", "" },
	{ "public.folder", "public.directory", "" }, { "public.volume", "public.folder", "" }, { "com.apple.package", "public.directory", "" }, { "com.apple.bundle", "public.directory", "" },
	{ "com.apple.application", "public.executable", "" }, { "com.apple.application-bundle", "com.apple.application com.apple.bundle com.apple.package", "app" },
	{ "com.apple.framework", "com.apple.bundle", "framework" }, { "public.executable", "public.data", "" }, { "public.unix-executable", "public.executable", "" },
	{ "com.apple.resolvable", "public.item", "" }, { "public.symlink", "public.data com.apple.resolvable", "" }, { "com.apple.alias-file", "public.data com.apple.resolvable", "" },
	{ "public.text", "public.data public.content", "" }, { "public.plain-text", "public.text", "txt" }, { "public.utf8-plain-text", "public.plain-text", "" },
	{ "public.xml", "public.text", "xml" }, { "com.apple.property-list", "public.xml", "plist" }, { "public.json", "public.text", "json" }, { "public.source-code", "public.text", "" },
	{ "public.image", "public.data public.content", "" }, { "public.png", "public.image", "png" }, { "public.jpeg", "public.image", "jpg" }, { "public.tiff", "public.image", "tiff" },
	{ "com.compuserve.gif", "public.image", "gif" }, { "com.apple.icns", "public.image", "icns" }, { "com.adobe.pdf", "public.data public.content", "pdf" },
	{ "public.audiovisual-content", "public.data public.content", "" }, { "public.audio", "public.audiovisual-content", "" }, { "public.movie", "public.audiovisual-content", "" },
	{ "public.mp3", "public.audio", "mp3" }, { "public.mpeg-4", "public.movie", "mp4" }, { "public.archive", "public.data", "" }, { "public.zip-archive", "public.archive", "zip" },
	{ "org.gnu.gnu-zip-archive", "public.archive", "gz" }, { "com.apple.disk-image", "public.data", "dmg" },
};
#define	NUT	(sizeof(uttab) / sizeof(uttab[0]))

@interface UTType : NSObject { @public NSString *_ident; }
+ (UTType *)typeWithIdentifier:(NSString *)i;
+ (UTType *)typeWithFilenameExtension:(NSString *)e;
+ (UTType *)typeWithFilenameExtension:(NSString *)e conformingToType:(UTType *)t;
+ (UTType *)typeWithMIMEType:(NSString *)m;
- (NSString *)identifier;
- (BOOL)conformsToType:(UTType *)t;
@end
@implementation UTType
+ (UTType *)typeWithIdentifier:(NSString *)i
{
	static NSMutableDictionary *cache; UTType *t;
	if (i == nil) return nil;
	if (cache == nil) cache = [[NSMutableDictionary alloc] init];
	if ((t = [cache objectForKey:i]) != nil) return t;
	t = [[UTType alloc] init]; t->_ident = [i copy]; [cache setObject:t forKey:i];
	return t;
}
+ (UTType *)typeWithFilenameExtension:(NSString *)e
{
	size_t k; NSString *l = [e lowercaseString];
	for (k = 0; k < NUT; k++) if (uttab[k].ext[0] && [l isEqualToString:[NSString stringWithUTF8String:uttab[k].ext]]) return [self typeWithIdentifier:[NSString stringWithUTF8String:uttab[k].id]];
	if ([l isEqualToString:@"jpeg"]) return [self typeWithIdentifier:@"public.jpeg"];
	if ([l length] == 0) return nil;
	return [self typeWithIdentifier:[@"dyn.ext." stringByAppendingString:l]];
}
+ (UTType *)typeWithFilenameExtension:(NSString *)e conformingToType:(UTType *)t
{
	UTType *r = [self typeWithFilenameExtension:e];
	return (t == nil || [r conformsToType:t]) ? r : nil;
}
+ (UTType *)typeWithMIMEType:(NSString *)m
{
	if ([m hasPrefix:@"image/"]) return [self typeWithIdentifier:@"public.image"];
	if ([m hasPrefix:@"text/"]) return [self typeWithIdentifier:@"public.text"];
	if ([m hasPrefix:@"audio/"]) return [self typeWithIdentifier:@"public.audio"];
	if ([m hasPrefix:@"video/"]) return [self typeWithIdentifier:@"public.movie"];
	return [self typeWithIdentifier:@"public.data"];
}
- (NSString *)identifier { return _ident; }
static BOOL
ut_conforms(NSString *from, NSString *to, int depth)
{
	size_t k;
	if ([from isEqualToString:to]) return YES;
	if (depth > 12) return NO;
	for (k = 0; k < NUT; k++) if ([from isEqualToString:[NSString stringWithUTF8String:uttab[k].id]]) {
		NSArray *ps = [[NSString stringWithUTF8String:uttab[k].parents] componentsSeparatedByString:@" "]; NSUInteger q;
		for (q = 0; q < [ps count]; q++) if ([[ps objectAtIndex:q] length] && ut_conforms([ps objectAtIndex:q], to, depth + 1)) return YES;
	}
	return NO;
}
- (BOOL)conformsToType:(UTType *)t { return t != nil && ut_conforms(_ident, [t identifier], 0); }
- (BOOL)isSupertypeOfType:(UTType *)t { return [t conformsToType:self]; }
- (BOOL)isSubtypeOfType:(UTType *)t { return [self conformsToType:t]; }
- (BOOL)isEqual:(id)o { return [o isKindOfClass:[UTType class]] && [_ident isEqualToString:((UTType *)o)->_ident]; }
- (NSUInteger)hash { return [_ident hash]; }
- (NSString *)description { return _ident; }
- (NSString *)preferredFilenameExtension { size_t k; for (k = 0; k < NUT; k++) if ([_ident isEqualToString:[NSString stringWithUTF8String:uttab[k].id]] && uttab[k].ext[0]) return [NSString stringWithUTF8String:uttab[k].ext]; return nil; }
- (NSString *)preferredMIMEType { return nil; }
- (NSString *)localizedDescription { return _ident; }
- (BOOL)isDynamic { return [_ident hasPrefix:@"dyn."]; }
- (BOOL)isDeclared { return ![self isDynamic]; }
- (id)copyWithZone:(NSZone *)z { return self; }
@end

/* the constant UTTypeXxx (a variable that holds a UTType): called by machrun for the symbol */
id
MachoUTConstant(const char *name)
{
	static const struct { const char *n, *i; } c[] = {
		{ "Application", "com.apple.application" }, { "ApplicationBundle", "com.apple.application-bundle" }, { "Bundle", "com.apple.bundle" }, { "Folder", "public.folder" },
		{ "Directory", "public.directory" }, { "Framework", "com.apple.framework" }, { "Resolvable", "com.apple.resolvable" }, { "Image", "public.image" }, { "Audio", "public.audio" },
		{ "Movie", "public.movie" }, { "Text", "public.text" }, { "Executable", "public.executable" }, { "PlainText", "public.plain-text" }, { "PNG", "public.png" }, { "JPEG", "public.jpeg" },
		{ "PDF", "com.adobe.pdf" }, { "Data", "public.data" }, { "Item", "public.item" }, { "Content", "public.content" }, { "Package", "com.apple.package" }, { "Archive", "public.archive" },
		{ "Symlink", "public.symlink" }, { "AliasFile", "com.apple.alias-file" }, { "UnixExecutable", "public.unix-executable" }, { "Volume", "public.volume" }, { "PropertyList", "com.apple.property-list" },
		{ "XML", "public.xml" }, { "JSON", "public.json" }, { "ZIP", "public.zip-archive" }, { "DiskImage", "com.apple.disk-image" },
	}; size_t k;
	for (k = 0; k < sizeof(c) / sizeof(c[0]); k++) if (strcmp(c[k].n, name) == 0) return [UTType typeWithIdentifier:[NSString stringWithUTF8String:c[k].i]];
	return [UTType typeWithIdentifier:[@"public." stringByAppendingString:[[NSString stringWithUTF8String:name] lowercaseString]]];
}

/* NSURLContentTypeKey for a file URL: what the file is on the disk */
static IMP o_getRV;
/* the size of a file, or of a folder with everything in it (st_blocks: what the file system allocates) */
static unsigned long long
tree_size(NSString *path, BOOL allocated)
{
	struct stat st; unsigned long long n;
	if (lstat([path fileSystemRepresentation], &st) != 0) return 0;
	n = allocated ? (unsigned long long)st.st_blocks * 512 : (unsigned long long)st.st_size;
	if (S_ISDIR(st.st_mode)) {
		NSArray *items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:path error:NULL]; NSUInteger i;
		for (i = 0; i < [items count]; i++) n += tree_size([path stringByAppendingPathComponent:[items objectAtIndex:i]], allocated);
	}
	return n;
}
static BOOL
w_getRV(id self, SEL _cmd, id *value, NSString *key, NSError **err)
{
	if (getenv("MACHRUN_DEBUG_RV")) fprintf(stderr, "machrun: getResourceValue key '%s' url %s\n", [key UTF8String], [[(NSURL *)self description] UTF8String]);
	if (([key isEqualToString:@"NSURLFileSizeKey"] || [key isEqualToString:@"NSURLTotalFileSizeKey"] || [key isEqualToString:@"NSURLFileAllocatedSizeKey"] || [key isEqualToString:@"NSURLTotalFileAllocatedSizeKey"]) && [(NSURL *)self isFileURL]) {
		NSString *path = [(NSURL *)self path]; struct stat st;
		if (path == nil || lstat([path fileSystemRepresentation], &st) != 0) return NO;
		{ BOOL total = [key hasPrefix:@"NSURLTotal"], alloc = [key rangeOfString:@"Allocated"].location != NSNotFound;
		  unsigned long long n = total ? tree_size(path, alloc) : (alloc ? (unsigned long long)st.st_blocks * 512 : (unsigned long long)st.st_size);
		  if (value) *value = [NSNumber numberWithUnsignedLongLong:n]; }
		return YES;
	}
	if ([key isEqualToString:@"NSURLContentTypeKey"] || [key isEqualToString:@"NSURLTypeIdentifierKey"]) {
		NSString *path = [(NSURL *)self path]; struct stat st; UTType *t = nil;
		if (path && [(NSURL *)self isFileURL] && lstat([path fileSystemRepresentation], &st) == 0) {
			if (S_ISLNK(st.st_mode)) t = [UTType typeWithIdentifier:@"public.symlink"];
			else if (S_ISDIR(st.st_mode)) {
				NSString *e = [[path pathExtension] lowercaseString];
				if ([e isEqualToString:@"app"]) t = [UTType typeWithIdentifier:@"com.apple.application-bundle"];
				else if ([e isEqualToString:@"framework"]) t = [UTType typeWithIdentifier:@"com.apple.framework"];
				else if ([e length] && ([[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"Contents/Info.plist"]] || [[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"Info.plist"]]))
					t = [UTType typeWithIdentifier:@"com.apple.bundle"];
				else t = [UTType typeWithIdentifier:@"public.folder"];
			} else { t = [UTType typeWithFilenameExtension:[path pathExtension]]; if (t == nil) t = [UTType typeWithIdentifier:@"public.data"]; }
		}
		if (getenv("MACHRUN_DEBUG_RV")) fprintf(stderr, "machrun:   path %s -> %s (value %p)\n", [path UTF8String], t ? [[t identifier] UTF8String] : "nil", (void *)value);
		if ([key isEqualToString:@"NSURLTypeIdentifierKey"]) { if (value) *value = [t identifier]; } else { if (value) *value = t; }
		return t != nil;
	}
	{	/* the keys that GNUstep does not give (the ones Mac programs ask first: folder or file, the volume with its capacity, the dates): when its own answer is no, one from stat and statfs */
		BOOL ok = ((BOOL (*)(id, SEL, id *, NSString *, NSError **))o_getRV)(self, _cmd, value, key, err);
		NSString *path; struct stat st; struct statfs sf; NSNumber *n = nil; id v = nil;
		if (ok) return ok;
		if (value) *value = nil;		/* a program that does not look at the answer (GrandPerspective logs the format of the volume) must not get what was in its variable */
		if (![(NSURL *)self isFileURL] || (path = [(NSURL *)self path]) == nil || lstat([path fileSystemRepresentation], &st) != 0) return NO;
		if ([key isEqualToString:@"NSURLIsDirectoryKey"]) n = [NSNumber numberWithBool:S_ISDIR(st.st_mode) != 0];
		else if ([key isEqualToString:@"NSURLIsRegularFileKey"]) n = [NSNumber numberWithBool:S_ISREG(st.st_mode) != 0];
		else if ([key isEqualToString:@"NSURLIsSymbolicLinkKey"]) n = [NSNumber numberWithBool:S_ISLNK(st.st_mode) != 0];
		else if ([key isEqualToString:@"NSURLIsHiddenKey"]) n = [NSNumber numberWithBool:[[path lastPathComponent] hasPrefix:@"."]];
		else if ([key isEqualToString:@"NSURLIsPackageKey"]) {
			NSString *e = [[path pathExtension] lowercaseString];
			n = [NSNumber numberWithBool:S_ISDIR(st.st_mode) && ([e isEqualToString:@"app"] || [e isEqualToString:@"framework"] || [e isEqualToString:@"bundle"] || [e isEqualToString:@"plugin"] || [e isEqualToString:@"kext"])];
		}
		else if ([key isEqualToString:@"NSURLIsVolumeKey"]) n = [NSNumber numberWithBool:[path isEqualToString:@"/"]];
		else if ([key isEqualToString:@"NSURLNameKey"] || [key isEqualToString:@"NSURLLocalizedNameKey"]) v = [path lastPathComponent];
		else if ([key isEqualToString:@"NSURLPathKey"]) v = path;
		else if ([key isEqualToString:@"NSURLContentModificationDateKey"] || [key isEqualToString:@"NSURLAttributeModificationDateKey"]) v = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)st.st_mtime];
		else if ([key isEqualToString:@"NSURLContentAccessDateKey"]) v = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)st.st_atime];
		else if ([key isEqualToString:@"NSURLCreationDateKey"]) v = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)st.st_birthtime];
		else if (statfs([path fileSystemRepresentation], &sf) == 0) {
			if ([key isEqualToString:@"NSURLVolumeURLKey"]) v = [NSURL fileURLWithPath:[NSString stringWithUTF8String:sf.f_mntonname] isDirectory:YES];
			else if ([key isEqualToString:@"NSURLVolumeTotalCapacityKey"]) n = [NSNumber numberWithLongLong:(long long)sf.f_blocks * (long long)sf.f_bsize];
			else if ([key isEqualToString:@"NSURLVolumeAvailableCapacityKey"] || [key isEqualToString:@"NSURLVolumeAvailableCapacityForImportantUsageKey"]) n = [NSNumber numberWithLongLong:(long long)sf.f_bavail * (long long)sf.f_bsize];
			else if ([key isEqualToString:@"NSURLVolumeNameKey"] || [key isEqualToString:@"NSURLVolumeLocalizedNameKey"]) v = [[NSString stringWithUTF8String:sf.f_mntfromname] lastPathComponent];
			else if ([key isEqualToString:@"NSURLVolumeLocalizedFormatDescriptionKey"]) v = [[NSString stringWithUTF8String:sf.f_fstypename] uppercaseString];
			else if ([key isEqualToString:@"NSURLVolumeIsLocalKey"]) n = [NSNumber numberWithBool:(sf.f_flags & MNT_LOCAL) != 0];
		}
		if (n != nil) v = n;
		if (v == nil) return NO;
		if (value) *value = v;
		return YES;
	}
}
__attribute__((constructor)) static void
uttype_init(void)
{
	if (getenv("MACHRUN_NOUT")) return;
	Method m; Class k = objc_getClass("NSURL");
	if (k == Nil) return;
	(void)[k class];
	if ((m = class_getInstanceMethod(k, @selector(getResourceValue:forKey:error:))) != NULL) o_getRV = method_setImplementation(m, (IMP)w_getRV);
}

/* NSViewController without a nib of its own (a controller that builds its view in code, like AppCleaner's HelpViewController): AppKit gives it an empty view, GNUstep raises an exception. */
static IMP o_loadView;
static void
w_loadView(id self, SEL _cmd)
{
	@try { ((void (*)(id, SEL))o_loadView)(self, _cmd); }
	@catch (NSException *e) {
		Class vk = NSClassFromString(@"NSView"); id v;
		if (getenv("MACHRUN_DEBUG")) fprintf(stderr, "machrun: loadView of %s: %s (an empty view is used)\n", class_getName(object_getClass(self)), [[e reason] UTF8String]);
		v = [vk alloc];
		v = ((id (*)(id, SEL, NSRect))class_getMethodImplementation(vk, @selector(initWithFrame:)))(v, @selector(initWithFrame:), NSMakeRect(0, 0, 300, 200));
		((void (*)(id, SEL, id))class_getMethodImplementation(object_getClass(self), @selector(setView:)))(self, @selector(setView:), [v autorelease]);
	}
}
__attribute__((constructor)) static void
viewcontroller_init(void)
{
	Class k = NSClassFromString(@"NSViewController"); Method m;
	if (k == Nil) return;
	(void)[k class];
	if ((m = class_getInstanceMethod(k, @selector(loadView))) != NULL) o_loadView = method_setImplementation(m, (IMP)w_loadView);
}

/* The folders of a Mac: GNUstep answers NSLibraryDirectory with ~/GNUstep/Library; a Mac application looks in ~/Library, /Library and /System/Library (AppCleaner looks for the files of an
 * application there). */
static IMP o_urlsForDir;
static id
w_urlsForDir(id self, SEL _cmd, NSUInteger dir, NSUInteger mask)
{
	NSString *home = NSHomeDirectory(); NSMutableArray *r = [NSMutableArray array]; NSString *sub = nil; BOOL sys = NO, apps = NO;
	switch (dir) {
	case 5: sub = @"Library"; sys = YES; break;
	case 14: sub = @"Library/Application Support"; break;
	case 13: sub = @"Library/Caches"; break;
	case 1: apps = YES; break;
	default: return ((id (*)(id, SEL, NSUInteger, NSUInteger))o_urlsForDir)(self, _cmd, dir, mask);
	}
	if (getenv("MACHRUN_DEBUG_RV")) fprintf(stderr, "machrun: URLsForDirectory %lu mask %lu -> home %s\n", (unsigned long)dir, (unsigned long)mask, [home UTF8String]);
	if (apps) {
		if (mask & 1) [r addObject:[NSURL fileURLWithPath:[home stringByAppendingPathComponent:@"Applications"] isDirectory:YES]];
		if (mask & 2) [r addObject:[NSURL fileURLWithPath:@"/Applications" isDirectory:YES]];
		return r;
	}
	if (mask & 1) [r addObject:[NSURL fileURLWithPath:[home stringByAppendingPathComponent:sub] isDirectory:YES]];
	if (mask & 2) [r addObject:[NSURL fileURLWithPath:[@"/" stringByAppendingString:sub] isDirectory:YES]];
	if ((mask & 8) && sys) [r addObject:[NSURL fileURLWithPath:@"/System/Library" isDirectory:YES]];
	return r;
}
__attribute__((constructor)) static void
dirs_init(void)
{
	if (getenv("MACHRUN_NODIRS")) return;
	Class k = NSClassFromString(@"NSFileManager"); Method m;
	if (k == Nil) return;
	(void)[k class];
	m = class_getInstanceMethod(k, @selector(URLsForDirectory:inDomains:));
	if (getenv("MACHRUN_DEBUG")) fprintf(stderr, "machrun: NSFileManager URLsForDirectory:inDomains: method %p\n", (void *)m);
	if (m != NULL) o_urlsForDir = method_setImplementation(m, (IMP)w_urlsForDir);
}

/* The Info.plist of a bundle of macOS (Contents/Info.plist): GNUstep reads Resources/Info-gnustep.plist (and the Info.plist of its own layout) only, so a Mac application that looks at the
 * bundle of another application (AppCleaner, the Finder ...) gets no bundle identifier and no name. */
static IMP o_infoDict, o_infoObj, o_bundleId;
#include <pthread.h>
static NSDictionary *
mac_info(id self)
{
	static NSMutableDictionary *cache; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	NSString *path = [(NSBundle *)self bundlePath], *f; NSDictionary *d, *r;
	if (path == nil) return nil;
	pthread_mutex_lock(&mu);
	if (cache == nil) cache = [[NSMutableDictionary alloc] init];
	if ((d = [cache objectForKey:path]) == nil) {
		f = [path stringByAppendingPathComponent:@"Contents/Info.plist"];
		d = [NSDictionary dictionaryWithContentsOfFile:f];
		if (d == nil) d = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
		[cache setObject:(d ? d : [NSDictionary dictionary]) forKey:path];
	}
	r = [d count] ? [[d retain] autorelease] : nil;
	pthread_mutex_unlock(&mu);
	return r;
}
static id
w_infoDict(id self, SEL _cmd)
{
	NSDictionary *d = ((id (*)(id, SEL))o_infoDict)(self, _cmd), *m;
	if ([d objectForKey:@"CFBundleIdentifier"] != nil) return d;
	if ((m = mac_info(self)) == nil) return d;
	if ([d count] == 0) return m;
	{ NSMutableDictionary *r = [NSMutableDictionary dictionaryWithDictionary:m]; [r addEntriesFromDictionary:d]; return r; }
}
static id
w_infoObj(id self, SEL _cmd, NSString *key)
{
	id v = ((id (*)(id, SEL, id))o_infoObj)(self, _cmd, key);
	return v != nil ? v : [mac_info(self) objectForKey:key];
}
static id
w_bundleId(id self, SEL _cmd)
{
	id v = ((id (*)(id, SEL))o_bundleId)(self, _cmd);
	return v != nil ? v : [mac_info(self) objectForKey:@"CFBundleIdentifier"];
}
__attribute__((constructor)) static void
bundleinfo_init(void)
{
	if (getenv("MACHRUN_NOBUNDLEINFO")) return;
	Class k = NSClassFromString(@"NSBundle"); Method m;
	if (k == Nil) return;
	(void)[k class];
	if ((m = class_getInstanceMethod(k, @selector(infoDictionary))) != NULL) o_infoDict = method_setImplementation(m, (IMP)w_infoDict);
	if ((m = class_getInstanceMethod(k, @selector(objectForInfoDictionaryKey:))) != NULL) o_infoObj = method_setImplementation(m, (IMP)w_infoObj);
	if ((m = class_getInstanceMethod(k, @selector(bundleIdentifier))) != NULL) o_bundleId = method_setImplementation(m, (IMP)w_bundleId);
}

/* This libdispatch calls _dispatch_ulock_wait and _dispatch_ulock_wake (the locks of Darwin's kernel) for dispatch_group_wait and the like, and leaves them undefined on FreeBSD: the program would end
 * at the first wait ("Undefined symbol").  They are made here with FreeBSD's umtx (what libdispatch itself uses elsewhere). */
#include <sys/umtx.h>
#include <errno.h>
#include <limits.h>
int
_dispatch_ulock_wait(uint32_t *uaddr, uint32_t val, uint32_t timeout_us, uint32_t flags)
{
	struct _umtx_time ut; void *ts = NULL; size_t sz = 0; int r;
	(void)flags;
	if (timeout_us != 0) { ut._timeout.tv_sec = timeout_us / 1000000; ut._timeout.tv_nsec = (long)(timeout_us % 1000000) * 1000; ut._flags = 0; ut._clockid = CLOCK_MONOTONIC; ts = &ut; sz = sizeof(ut); }
	r = _umtx_op(uaddr, UMTX_OP_WAIT_UINT_PRIVATE, val, (void *)sz, ts);
	return r == 0 ? 0 : -errno;
}
int
_dispatch_ulock_wake(uint32_t *uaddr, uint32_t flags)
{
	int r = _umtx_op(uaddr, UMTX_OP_WAKE_PRIVATE, (flags & 0x100) ? INT_MAX : 1, NULL, NULL);
	return r == 0 ? 0 : -errno;
}

/* NSFileManager enumeratorAtURL:...: GNUstep's enumerator makes the URLs of what it finds from the relative names (resolved against the working directory, not against the folder that is
 * enumerated).  The folder is remembered for the enumerator and the URLs are made from it. */
#include <objc/runtime.h>
static IMP o_enumAtURL, o_urlEnumNext;
static char enum_root_key;
static id
w_enumAtURL(id self, SEL _cmd, id url, id keys, NSUInteger opts, id handler)
{
	id e = ((id (*)(id, SEL, id, id, NSUInteger, id))o_enumAtURL)(self, _cmd, url, keys, opts, handler);
	if (e != nil && url != nil) objc_setAssociatedObject(e, &enum_root_key, [(NSURL *)url path], OBJC_ASSOCIATION_RETAIN);
	return e;
}
static id
w_urlEnumNext(id self, SEL _cmd)
{
	id u = ((id (*)(id, SEL))o_urlEnumNext)(self, _cmd);
	NSString *root = objc_getAssociatedObject(self, &enum_root_key), *p;
	if (u == nil || root == nil) return u;
	p = [(NSURL *)u path];
	{ NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
	  if (cwd != nil && [p hasPrefix:cwd] && ![p hasPrefix:root]) {
		NSString *rel = [p substringFromIndex:[cwd length]];
		while ([rel hasPrefix:@"/"]) rel = [rel substringFromIndex:1];
		return [NSURL fileURLWithPath:[root stringByAppendingPathComponent:rel] isDirectory:[[(NSURL *)u absoluteString] hasSuffix:@"/"]];
	  } }
	return u;
}
__attribute__((constructor)) static void
urlenum_init(void)
{
	if (getenv("MACHRUN_NOURLENUM")) return;
	Class k = NSClassFromString(@"NSFileManager"), e; Method m;
	if (k == Nil) return;
	(void)[k class];
	if ((m = class_getInstanceMethod(k, @selector(enumeratorAtURL:includingPropertiesForKeys:options:errorHandler:))) != NULL) o_enumAtURL = method_setImplementation(m, (IMP)w_enumAtURL);
	if ((e = NSClassFromString(@"GSURLEnumerator")) != Nil) { (void)[e class]; if ((m = class_getInstanceMethod(e, @selector(nextObject))) != NULL) o_urlEnumNext = method_setImplementation(m, (IMP)w_urlEnumNext); }
}

/* -[NSFileManager trashItemAtURL:resultingItemURL:error:] (macOS 10.8): the file goes to ~/.Trash (the name is made unique when it is there already) */
static BOOL
fm_trashItem(id self, SEL _cmd, NSURL *url, NSURL **result, NSError **error)
{
	NSFileManager *fm = [NSFileManager defaultManager]; NSString *trash = [NSHomeDirectory() stringByAppendingPathComponent:@".Trash"], *name, *dst; int n = 1;
	if (url == nil) return NO;
	[fm createDirectoryAtPath:trash withIntermediateDirectories:YES attributes:nil error:NULL];
	name = [[url path] lastPathComponent]; dst = [trash stringByAppendingPathComponent:name];
	while ([fm fileExistsAtPath:dst]) dst = [trash stringByAppendingPathComponent:[NSString stringWithFormat:@"%@ %d.%@", [name stringByDeletingPathExtension], ++n, [name pathExtension]]];
	if (![fm moveItemAtPath:[url path] toPath:dst error:error]) return NO;
	if (result) *result = [NSURL fileURLWithPath:dst];
	return YES;
}
__attribute__((constructor)) static void
trash_init(void)
{
	if (getenv("MACHRUN_NOTRASH")) return;
	Class k = NSClassFromString(@"NSFileManager");
	if (k == Nil) return;
	(void)[k class];
	class_addMethod(k, @selector(trashItemAtURL:resultingItemURL:error:), (IMP)fm_trashItem, "c40@0:8@16^@24^@32");
}


/* Plug-ins that are Mach-O bundles (XLD's output and decoder plug-ins): -[NSBundle load] asks machrun to load the executable of the bundle (Contents/MacOS/NAME); the principal class
 * is the class that the Info.plist names. */
static void *(*macho_loader)(const char *);
static IMP o_bundleLoad, o_bundlePrincipal, o_bundleIsLoaded;
static NSString *
macho_exec(id self)
{
	NSDictionary *info = [(NSBundle *)self infoDictionary]; NSString *name = [info objectForKey:@"CFBundleExecutable"], *p;
	if (name == nil) return nil;
	p = [[(NSBundle *)self bundlePath] stringByAppendingPathComponent:[@"Contents/MacOS" stringByAppendingPathComponent:name]];
	if (access([p fileSystemRepresentation], R_OK) != 0) return nil;
	{ FILE *f = fopen([p fileSystemRepresentation], "r"); unsigned char m[4] = { 0 }; int ok;
	  if (f == NULL) return nil;
	  ok = fread(m, 1, 4, f) == 4 && ((m[0] == 0xca && m[1] == 0xfe && m[2] == 0xba && m[3] == 0xbe) || (m[0] == 0xcf && m[1] == 0xfa && m[2] == 0xed && m[3] == 0xfe));
	  fclose(f);
	  return ok ? p : nil; }
}
static NSMutableSet *macho_loaded;
static BOOL
w_bundleLoad(id self, SEL _cmd)
{
	NSString *p;
	if (getenv("MACHRUN_DEBUG")) fprintf(stderr, "machrun: -[NSBundle load] %s (loader %p, Mach-O executable %s)\n", [[(NSBundle *)self bundlePath] UTF8String], (void *)macho_loader, macho_loader ? [macho_exec(self) UTF8String] : "-");
	if (macho_loader == NULL || (p = macho_exec(self)) == nil) return ((BOOL (*)(id, SEL))o_bundleLoad)(self, _cmd);
	@synchronized(macho_loaded) {
		if ([macho_loaded containsObject:p]) return YES;
		if (macho_loader([p fileSystemRepresentation]) == NULL) return NO;
		[macho_loaded addObject:p];
	}
	return YES;
}
static BOOL
w_bundleIsLoaded(id self, SEL _cmd)
{
	NSString *p = macho_loader ? macho_exec(self) : nil;
	if (p != nil) return [macho_loaded containsObject:p];
	return ((BOOL (*)(id, SEL))o_bundleIsLoaded)(self, _cmd);
}
static Class
w_bundlePrincipal(id self, SEL _cmd)
{
	if (macho_loader && macho_exec(self) != nil) {
		NSString *cn = [[(NSBundle *)self infoDictionary] objectForKey:@"NSPrincipalClass"];
		if (!w_bundleIsLoaded(self, _cmd) && !w_bundleLoad(self, @selector(load))) return Nil;
		return cn ? NSClassFromString(cn) : Nil;
	}
	return ((Class (*)(id, SEL))o_bundlePrincipal)(self, _cmd);
}
__attribute__((constructor)) static void
machobundle_init(void)
{
	Class k = NSClassFromString(@"NSBundle"); Method m; const char *a = getenv("MACHRUN_LOADER_FN");
	if (k == Nil) return;
	(void)[k class];
	macho_loaded = [[NSMutableSet alloc] init];
	if (a) macho_loader = (void *(*)(const char *))strtoul(a, NULL, 16);
	if ((m = class_getInstanceMethod(k, @selector(load))) != NULL) o_bundleLoad = method_setImplementation(m, (IMP)w_bundleLoad);
	if ((m = class_getInstanceMethod(k, @selector(isLoaded))) != NULL) o_bundleIsLoaded = method_setImplementation(m, (IMP)w_bundleIsLoaded);
	if ((m = class_getInstanceMethod(k, @selector(principalClass))) != NULL) o_bundlePrincipal = method_setImplementation(m, (IMP)w_bundlePrincipal);
}


/* -[NSView registerForDraggedTypes:] while the view is decoded from a nib and has no window yet (an NSPathControl of Sloth's window): GNUstep dereferences the window's server and ends the program.
 * The registration is dropped then; a view that is put in a window and wants the drops registers again (the drops are not what the programs here need). */
static IMP o_registerForDraggedTypes;
static void
w_registerForDraggedTypes(id self, SEL _cmd, NSArray *types)
{
	if (((id (*)(id, SEL))objc_msgSend)(self, @selector(window)) == nil) return;
	((void (*)(id, SEL, id))o_registerForDraggedTypes)(self, _cmd, types);
}
__attribute__((constructor)) static void
dragged_types_init(void)
{
	Class k = NSClassFromString(@"NSView"); Method m;
	if (k == Nil || getenv("MACHRUN_NODRAGFIX")) return;
	if ((m = class_getInstanceMethod(k, @selector(registerForDraggedTypes:))) != NULL) o_registerForDraggedTypes = method_setImplementation(m, (IMP)w_registerForDraggedTypes);
}


/* Conversions between views that are not in one window yet (what a table's header does while its nib is decoded): GNUstep asserts (NSView.m:1846 "convertPoint:toView:") and the whole nib is lost.
 * Apple's answer for views without a window is a conversion through the superviews; here the point or the rectangle comes back as it was when the assertion fires. */
static IMP o_cptv, o_cpfv, o_crtv, o_crfv;
static NSPoint w_cptv(id self, SEL _cmd, NSPoint p, id v) { @try { return ((NSPoint (*)(id, SEL, NSPoint, id))o_cptv)(self, _cmd, p, v); } @catch (id e) { return p; } }
static NSPoint w_cpfv(id self, SEL _cmd, NSPoint p, id v) { @try { return ((NSPoint (*)(id, SEL, NSPoint, id))o_cpfv)(self, _cmd, p, v); } @catch (id e) { return p; } }
static NSRect w_crtv(id self, SEL _cmd, NSRect r, id v) { @try { return ((NSRect (*)(id, SEL, NSRect, id))o_crtv)(self, _cmd, r, v); } @catch (id e) { return r; } }
static NSRect w_crfv(id self, SEL _cmd, NSRect r, id v) { @try { return ((NSRect (*)(id, SEL, NSRect, id))o_crfv)(self, _cmd, r, v); } @catch (id e) { return r; } }
__attribute__((constructor)) static void
convert_guard_init(void)
{
	Class k = NSClassFromString(@"NSView"); Method m;
	if (k == Nil || getenv("MACHRUN_NOCONVERTFIX")) return;
	if ((m = class_getInstanceMethod(k, @selector(convertPoint:toView:))) != NULL) o_cptv = method_setImplementation(m, (IMP)w_cptv);
	if ((m = class_getInstanceMethod(k, @selector(convertPoint:fromView:))) != NULL) o_cpfv = method_setImplementation(m, (IMP)w_cpfv);
	if ((m = class_getInstanceMethod(k, @selector(convertRect:toView:))) != NULL) o_crtv = method_setImplementation(m, (IMP)w_crtv);
	if ((m = class_getInstanceMethod(k, @selector(convertRect:fromView:))) != NULL) o_crfv = method_setImplementation(m, (IMP)w_crfv);
}


/* NSWindowController finds its nib from the ivar that -initWithWindowNibName: sets; a Mac program that overrides -windowNibName (a subclass that answers the name itself: GrandPerspective's start window)
 * has no name in the ivar and GNUstep loads a nib of nil.  Without a path of its own, the controller asks -windowNibName and looks the nib up as GNUstep does. */
static IMP o_windowNibPath;
static id
w_windowNibPath(id self, SEL _cmd)
{
	id r = ((id (*)(id, SEL))o_windowNibPath)(self, _cmd), name, owner, b;
	if (r != nil) return r;
	name = ((id (*)(id, SEL))objc_msgSend)(self, @selector(windowNibName));
	if (name == nil) return nil;
	owner = ((id (*)(id, SEL))objc_msgSend)(self, @selector(owner));
	b = [NSBundle bundleForClass:[owner class]];
	r = b != nil ? ((id (*)(id, SEL, id))objc_msgSend)(b, @selector(pathForNibResource:), name) : nil;
	if (r == nil) r = ((id (*)(id, SEL, id))objc_msgSend)([NSBundle mainBundle], @selector(pathForNibResource:), name);
	return r;
}
__attribute__((constructor)) static void
windownib_init(void)
{
	Class k = NSClassFromString(@"NSWindowController"); Method m;
	if (k == Nil || getenv("MACHRUN_NOWINNIBFIX")) return;
	(void)[k class];
	if ((m = class_getInstanceMethod(k, @selector(windowNibPath))) != NULL) o_windowNibPath = method_setImplementation(m, (IMP)w_windowNibPath);
}


/* UTTypeCopyDeclaration: a declaration for every identifier (the programs that build a list of the file types of a folder, GrandPerspective, drop a type whose declaration is missing and then add nil to a set).
 * The keys are what the program has for kUTTypeDescriptionKey and kUTTypeConformsToKey: strings with the names of those constants (see objc.c, constants that no library has). */
CFTypeRef UTTypeCopyDeclaration(CFStringRef uti);
CFTypeRef
UTTypeCopyDeclaration(CFStringRef uti)
{
	NSString *u = (NSString *)uti, *desc; NSArray *conf;
	if (getenv("MACHRUN_DEBUG_UT")) fprintf(stderr, "machrun: UTTypeCopyDeclaration(%s)\n", u ? [u UTF8String] : "nil");
	if (u == nil) return NULL;
	if ([u isEqualToString:@"public.data"]) { desc = @"Data"; conf = [NSArray arrayWithObject:@"public.item"]; }
	else if ([u isEqualToString:@"public.item"]) { desc = @"Item"; conf = [NSArray array]; }
	else if ([u hasPrefix:@"dyn."]) { desc = [NSString stringWithFormat:@"%@ document", [u substringFromIndex:4]]; conf = [NSArray arrayWithObject:@"public.data"]; }
	else { desc = u; conf = [NSArray arrayWithObject:@"public.data"]; }
	return (CFTypeRef)[[NSDictionary dictionaryWithObjectsAndKeys:desc, @"kUTTypeDescriptionKey", conf, @"kUTTypeConformsToKey", nil] retain];
}


/* UTTypeCopyPreferredTagWithClass: the extension of a "dyn." identifier (what UTTypeCreatePreferredIdentifierForTag made of it); nothing for the others */
CFTypeRef UTTypeCopyPreferredTagWithClass(CFStringRef uti, CFStringRef tagClass);
CFTypeRef
UTTypeCopyPreferredTagWithClass(CFStringRef uti, CFStringRef tagClass)
{
	NSString *u = (NSString *)uti;
	if (getenv("MACHRUN_DEBUG_UT")) fprintf(stderr, "machrun: UTTypeCopyPreferredTagWithClass(%s)\n", u ? [u UTF8String] : "nil");
	(void)tagClass;
	return (u != nil && [u hasPrefix:@"dyn."] && [u length] > 4) ? (CFTypeRef)[[u substringFromIndex:4] retain] : NULL;
}

/* +[NSBundle bundleForClass:] of a class that a Mach-O image defines (a plug-in, a framework of the application): the bundle that holds the image, not a library of GNUstep */
static const char *(*class_image_path)(Class);
static IMP o_bundleForClass;
static id
w_bundleForClass(id self, SEL _cmd, Class k)
{
	const char *p = class_image_path ? class_image_path(k) : NULL;
	if (p != NULL) {
		NSString *s = [NSString stringWithUTF8String:p]; NSArray *parts = [s pathComponents]; NSInteger i;
		for (i = (NSInteger)[parts count] - 1; i >= 0; i--) {
			NSString *c = [parts objectAtIndex:i];
			if ([c hasSuffix:@".bundle"] || [c hasSuffix:@".framework"] || [c hasSuffix:@".app"]) {
				NSString *dir = [NSString pathWithComponents:[parts subarrayWithRange:NSMakeRange(0, i + 1)]];
				if ([c hasSuffix:@".app"] && [c isEqualToString:[[[NSBundle mainBundle] bundlePath] lastPathComponent]])
					return [NSBundle mainBundle];	/* the program's own classes: the wrapped bundle (with the English.lproj that merges Base.lproj), not the Mac layout it was wrapped from */
				id b = [NSBundle bundleWithPath:dir];
				if (b != nil) return b;
				break;
			}
		}
	}
	return ((id (*)(id, SEL, Class))o_bundleForClass)(self, _cmd, k);
}
__attribute__((constructor)) static void
bundleforclass_init(void)
{
	Class k = NSClassFromString(@"NSBundle"); Method m; const char *a = getenv("MACHRUN_CLASSPATH_FN");
	if (k == Nil) return;
	(void)[k class];
	if (a) class_image_path = (const char *(*)(Class))strtoul(a, NULL, 16);
	if ((m = class_getClassMethod(k, @selector(bundleForClass:))) != NULL) o_bundleForClass = method_setImplementation(m, (IMP)w_bundleForClass);
}

/* The resources of a bundle in the layout of macOS (NAME.bundle/Contents/Resources): GNUstep looks in NAME.bundle/Resources */
static IMP o_resourcePath, o_pathForResource3;
static NSString *
mac_resources(id self)
{
	NSString *r = [[(NSBundle *)self bundlePath] stringByAppendingPathComponent:@"Contents/Resources"]; BOOL d = NO;
	return ([[NSFileManager defaultManager] fileExistsAtPath:r isDirectory:&d] && d) ? r : nil;
}
static NSString *
w_resourcePath(id self, SEL _cmd)
{
	NSString *r = mac_resources(self);
	return r != nil ? r : ((id (*)(id, SEL))o_resourcePath)(self, _cmd);
}
static NSString *
w_pathForResource(id self, SEL _cmd, NSString *name, NSString *ext, NSString *dir)
{
	NSString *base = mac_resources(self);
	if (base != nil && name != nil) {
		NSFileManager *fm = [NSFileManager defaultManager]; NSString *file = [ext length] ? [name stringByAppendingPathExtension:ext] : name, *root = [dir length] ? [base stringByAppendingPathComponent:dir] : base, *c;
		NSArray *langs = [NSArray arrayWithObjects:@"", @"en.lproj", @"English.lproj", @"Base.lproj", nil]; NSUInteger i;
		for (i = 0; i < [langs count]; i++) {
			NSString *l = [langs objectAtIndex:i];
			c = [l length] ? [[root stringByAppendingPathComponent:l] stringByAppendingPathComponent:file] : [root stringByAppendingPathComponent:file];
			if ([fm fileExistsAtPath:c]) return c;
		}
	}
	return ((id (*)(id, SEL, id, id, id))o_pathForResource3)(self, _cmd, name, ext, dir);
}
static NSString *
w_pathForResource2(id self, SEL _cmd, NSString *name, NSString *ext) { return w_pathForResource(self, @selector(pathForResource:ofType:inDirectory:), name, ext, nil); }
__attribute__((constructor)) static void
macresources_init(void)
{
	Class k = NSClassFromString(@"NSBundle"); Method m;
	if (k == Nil) return;
	(void)[k class];
	if ((m = class_getInstanceMethod(k, @selector(resourcePath))) != NULL) o_resourcePath = method_setImplementation(m, (IMP)w_resourcePath);
	if ((m = class_getInstanceMethod(k, @selector(pathForResource:ofType:inDirectory:))) != NULL) o_pathForResource3 = method_setImplementation(m, (IMP)w_pathForResource);
	if ((m = class_getInstanceMethod(k, @selector(pathForResource:ofType:))) != NULL) method_setImplementation(m, (IMP)w_pathForResource2);
}

/* AppKit before the application object: a command line program of a Mac application (XLD --cmdline) loads a nib or asks for fonts without NSApplication; AppKit of macOS works that way, GNUstep
 * needs the shared application (it starts the backend) first. */
static IMP o_fontEnumShared;
static id
w_fontEnumShared(id self, SEL _cmd)
{
	static BOOL once;
	if (!once) {
		Class ak = NSClassFromString(@"NSApplication");
		once = YES;
		if (ak != Nil) ((id (*)(id, SEL))class_getMethodImplementation(object_getClass(ak), @selector(sharedApplication)))(ak, @selector(sharedApplication));
	}
	return ((id (*)(id, SEL))o_fontEnumShared)(self, _cmd);
}
__attribute__((constructor)) static void
fontenum_init(void)
{
	Class k = NSClassFromString(@"GSFontEnumerator"); Method m;
	if (k == Nil) return;
	(void)[k class];
	if ((m = class_getClassMethod(k, @selector(sharedEnumerator))) != NULL) o_fontEnumShared = method_setImplementation(m, (IMP)w_fontEnumShared);
}

/* -[NSView animator] and -[NSWindow animator] (the proxy whose setters animate): here the proxy is the object itself, so animator.frame = x sets the frame at once.  AppCleaner pushes its list view in
 * from x = 450 with it: without this the view stays outside of the window. */
static id anim_self(id self, SEL _cmd) { return self; }
static void anim_setanimations(id self, SEL _cmd, id d) { }
__attribute__((constructor)) static void
animator_init(void)
{
	const char *names[] = { "NSView", "NSWindow", "NSControl", "NSLayoutConstraint" }; int i;
	for (i = 0; i < 4; i++) {
		Class k = NSClassFromString([NSString stringWithUTF8String:names[i]]);
		if (k == Nil) continue;
		(void)[k class];
		if (getenv("MACHRUN_DEBUG")) fprintf(stderr, "machrun: animator of %s: %s\n", names[i], class_getInstanceMethod(k, @selector(animator)) ? "replaced" : "added");
		{ Method am = class_getInstanceMethod(k, @selector(animator)); if (am != NULL) method_setImplementation(am, (IMP)anim_self); else class_addMethod(k, @selector(animator), (IMP)anim_self, "@16@0:8"); }
		class_addMethod(k, @selector(setAnimations:), (IMP)anim_setanimations, "v24@0:8@16");
	}
}
