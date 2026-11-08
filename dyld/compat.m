/*
 * libmachcompat.so: GNUstep fixes written in Objective-C (they need @try), loaded by machrun when a program has Objective-C.
 * Each one is a place where Apple's AppKit is more forgiving than GNUstep's.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>

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

/* Sparkle (the update framework that Mac applications carry; its updater is an object of the main nib): there are no updates for programs that run here, so the updater is an object that does nothing. */
@interface SUUpdater : NSObject
+ (SUUpdater *)sharedUpdater;
- (IBAction)checkForUpdates:(id)sender;
@end
@implementation SUUpdater
+ (SUUpdater *)sharedUpdater { static SUUpdater *u; if (u == nil) u = [[SUUpdater alloc] init]; return u; }
- (id)initWithCoder:(NSCoder *)c { return [super init]; }
- (void)encodeWithCoder:(NSCoder *)c { }
- (void)checkForUpdates:(id)sender { }
- (void)checkForUpdatesInBackground { }
- (BOOL)automaticallyChecksForUpdates { return NO; }
- (void)setAutomaticallyChecksForUpdates:(BOOL)b { }
- (void)setDelegate:(id)d { }
- (id)delegate { return nil; }
- (void)setFeedURL:(NSURL *)u { }
- (BOOL)updateInProgress { return NO; }
@end

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
	Class k = NSClassFromString(@"NSImage"); Method m;
	if (k == Nil) return;
	(void)[k class];
	class_addMethod(object_getClass(k), @selector(imageWithSystemSymbolName:accessibilityDescription:), (IMP)symbol_image, "@32@0:8@16@24");
	class_addMethod(object_getClass(k), @selector(imageWithSystemSymbolName:variableValue:accessibilityDescription:), (IMP)symbol_image, "@40@0:8@16d24@32");
	if ((m = class_getClassMethod(k, @selector(imageNamed:))) != NULL) o_imageNamed = method_setImplementation(m, (IMP)w_imageNamed);
}
