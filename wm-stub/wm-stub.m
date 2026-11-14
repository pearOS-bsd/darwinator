/* wm-stub: the server side of com.apple.windowmanager.server (WindowManager.app of macOS 26) for pearOS.
 * AppKit 26 orders, shows and moves its windows through WindowManagement.framework: WMWindowTransaction actions travel over BoardServices
 * (BSXPC) to this service, which applies them to the window server with its own privileges.  Built on a Mac:
 *   cc -arch x86_64 -framework Foundation -o wm-stub wm-stub.m
 * Everything is reached by the runtime (the private classes have no headers here). */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
#import <stdio.h>

typedef void (^cfgblk)(id);

static void L(NSString *fmt, ...) { va_list ap; va_start(ap, fmt); NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap]; va_end(ap); fprintf(stderr, "wm-stub: %s\n", s.UTF8String); }

extern void objc_exception_throw(id);
static void my_throw(id e) { L(@"THROW %@ | %@", e, [NSThread callStackSymbols]); objc_exception_throw(e); }
__attribute__((used)) static struct { const void *a, *b; } ip_throw __attribute__((section("__DATA,__interpose"))) = { (const void *)my_throw, (const void *)objc_exception_throw };


/* a generic dump of an object: its properties by KVC (the classes are private) */
static NSString *dumpobj(id o)
{
	if (!o) return @"(nil)";
	NSMutableString *r = [NSMutableString stringWithFormat:@"<%s", class_getName([o class])];
	for (Class k = [o class]; k && k != [NSObject class]; k = class_getSuperclass(k)) {
		unsigned n = 0; Ivar *iv = class_copyIvarList(k, &n);
		for (unsigned i = 0; i < n; i++) {
			const char *nm = ivar_getName(iv[i]), *ty = ivar_getTypeEncoding(iv[i]);
			ptrdiff_t off = ivar_getOffset(iv[i]); char *base = (char *)(__bridge void *)o + off;
			NSString *v = @"?";
			switch (ty[0]) {
			case '@': { id x = *(id *)base; v = x ? [NSString stringWithFormat:@"%@", x] : @"nil"; break; }
			case 'q': case 'Q': v = [NSString stringWithFormat:@"%lld", *(long long *)base]; break;
			case 'i': case 'I': v = [NSString stringWithFormat:@"%d", *(int *)base]; break;
			case 'c': case 'B': v = [NSString stringWithFormat:@"%d", *(char *)base]; break;
			case 'd': v = [NSString stringWithFormat:@"%g", *(double *)base]; break;
			case '{': { unsigned long sz = 0, al = 0; NSGetSizeAndAlignment(ty, &sz, &al); NSMutableString *h = [NSMutableString string]; for (unsigned long b = 0; b < sz && b < 40; b++) [h appendFormat:@"%02x", (unsigned char)base[b]]; v = [NSString stringWithFormat:@"%s:%@", ty, h]; if (!strncmp(ty, "{CGRect", 7) || !strncmp(ty, "{CGPoint", 8) || !strncmp(ty, "{CGSize", 7)) v = [NSString stringWithFormat:@"%g,%g,%g,%g", ((double *)base)[0], ((double *)base)[1], ((double *)base)[2], ((double *)base)[3]]; break; }
			default: v = [NSString stringWithFormat:@"(%s)", ty];
			}
			if ([v isEqualToString:@"nil"] || [v isEqualToString:@"0"] || [v hasPrefix:@"{CPSProcessSerNum"] || [v hasPrefix:@"{CPSProcessSerNum="] || [v rangeOfString:@"0000000000000000"].location == 0) continue;
			[r appendFormat:@" %s=%@", nm, v];
		}
		free(iv);
	}
	[r appendString:@">"]; return r;
}

static void describe_action(id a)
{
	@try {
		L(@"  action type=%ld windowIDs=%@ spaceID=%llu props=%@ ordering=%@", (long)(NSInteger)[a performSelector:NSSelectorFromString(@"actionType")],
		  [a performSelector:NSSelectorFromString(@"windowIDs")], (unsigned long long)(NSUInteger)[a performSelector:NSSelectorFromString(@"spaceID")],
		  dumpobj([a performSelector:NSSelectorFromString(@"properties")]), dumpobj([a performSelector:NSSelectorFromString(@"orderingInfo")]));
	} @catch (id e) { L(@"  action: %@ (%@)", a, e); }
}


/* the window server: the stub is a client of it (the window server lets the WindowManager order the windows of everyone) */
static int (*p_cid)(void), (*p_order)(int, uint32_t, int, uint32_t);
static void *(*p_tcreate)(int);
static int (*p_torder)(void *, uint32_t, int, uint32_t);
static int (*p_tcommit)(void *, int);
typedef struct { double x, y, w, h; } WRect;
static WRect (*p_dbounds)(uint32_t); static uint32_t (*p_mainid)(void);
static int (*p_newrgn)(const WRect *, void **), (*p_setshape)(int, uint32_t, float, float, void *), (*p_relrgn)(void *);
static NSMutableDictionary *frames;		/* window identifier -> NSValue of the frame (AppKit's: the origin is the lower left) */
static NSMutableDictionary *wids;		/* window identifier (UUID) -> CGWindowID */
static void sls_init(void)
{
	static int done; if (done) return; done = 1;
	dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW);
	void (*ini)(void) = dlsym(RTLD_DEFAULT, "CGSInitialize"); if (ini) ini();
	p_cid = dlsym(RTLD_DEFAULT, "SLSMainConnectionID"); p_order = dlsym(RTLD_DEFAULT, "SLSOrderWindow");
	p_tcreate = dlsym(RTLD_DEFAULT, "SLSTransactionCreate"); p_torder = dlsym(RTLD_DEFAULT, "SLSTransactionOrderWindow"); p_tcommit = dlsym(RTLD_DEFAULT, "SLSTransactionCommit");
	p_dbounds = dlsym(RTLD_DEFAULT, "CGDisplayBounds"); p_mainid = dlsym(RTLD_DEFAULT, "CGMainDisplayID");
	p_newrgn = dlsym(RTLD_DEFAULT, "CGSNewRegionWithRect"); p_setshape = dlsym(RTLD_DEFAULT, "SLSSetWindowShape"); p_relrgn = dlsym(RTLD_DEFAULT, "CGSReleaseRegion");
	wids = [NSMutableDictionary new]; frames = [NSMutableDictionary new];
	L(@"skylight: cid=%d order=%p", p_cid ? p_cid() : -1, p_order);
}
static void apply_frame(NSString *u)
{
	NSNumber *w = wids[u]; NSValue *fv = frames[u]; WRect f;
	if (!w || !fv || !p_newrgn || !p_setshape) return;
	[fv getValue:&f];
	if (f.w < 1 || f.h < 1) return;
	double H = p_dbounds && p_mainid ? p_dbounds(p_mainid()).h : 800;
	WRect r = { 0, 0, f.w, f.h }; void *rg = NULL;
	int rc = p_newrgn(&r, &rg);
	if (rc == 0 && rg) { rc = p_setshape(p_cid(), [w unsignedIntValue], (float)f.x, (float)(H - (f.y + f.h)), rg); if (p_relrgn) p_relrgn(rg); }
	L(@"frame window %@: %g,%g %gx%g (screen height %g) -> %d", w, f.x, H - (f.y + f.h), f.w, f.h, H, rc);
}
static void apply_action(id a)
{
	long type = (long)(NSInteger)[a performSelector:NSSelectorFromString(@"actionType")];
	id props = [a performSelector:NSSelectorFromString(@"properties")];
	if (type == 2 || type == 1) {
		NSNumber *w = [props valueForKey:@"cgWindowID"]; NSString *u = [props valueForKey:@"windowIdentifier"];
		if (w && u && [w unsignedIntValue]) { wids[u] = w; L(@"window %@ = %@", u, w); apply_frame(u); }
		if (u) { NSValue *fv = [props valueForKey:@"frame"]; WRect f = {0}; if (fv) [fv getValue:&f]; if (fv && f.w >= 1 && f.h >= 1) { frames[u] = fv; apply_frame(u); } }
	}
	if (type == 4) {
		id oi = [a performSelector:NSSelectorFromString(@"orderingInfo")];
		NSString *u = [oi valueForKey:@"windowID"]; NSNumber *pos = [oi valueForKey:@"relativePosition"]; NSNumber *w = wids[u];
		if (!w) { L(@"order: unknown window %@", u); return; }
		int mode = [pos intValue] == 0 ? 0 : 1;			/* 0: out; the rest: in, above everything (the relative positions of WindowManagement are not decoded yet) */
		void *t = p_tcreate ? p_tcreate(p_cid()) : NULL; long rc = -1;
		if (t) { p_torder(t, [w unsignedIntValue], mode, 0); rc = (long)p_tcommit(t, 0); }
		L(@"order window %@ mode %d (position %@) -> %ld", w, mode, pos, rc);
		apply_frame(u);			/* a window that is not mapped may not take a shape: again, now that it is ordered in */
	}
}

@interface WMSrv : NSObject
@end
@implementation WMSrv
- (void)handleXPCWindowTransaction:(id)t completionHandler:(void (^)(void))h
{
	L(@"handleXPCWindowTransaction: %@", t);
	sls_init();
	for (id a in [t performSelector:NSSelectorFromString(@"actions")]) { describe_action(a); apply_action(a); }
	if (h) h();
}
- (void)retrieveStagesWithReply:(void (^)(id, id))h { L(@"retrieveStagesWithReply"); @try { if (getenv("WM_REPLY")) { h(@[], nil); L(@"stages replied"); } else L(@"stages: not replying"); } @catch (id e) { L(@"EXCEPTION in reply: %@", e); } }
- (void)performAsynchronousBridgedWindowManagementOperation:(id)op { L(@"async bridged op: %@", op); }
- (void)performSynchronousBridgedWindowManagementOperation:(id)op replyHandler:(void (^)(id))h { L(@"sync bridged op: %@", op); if (h) h(nil); }
- (void)proposeKeyWindowWithInfo:(id)i reply:(void (^)(id))h { L(@"proposeKey %@", i); if (h) h(nil); }
- (void)proposeMainWindowWithInfo:(id)i reply:(void (^)(id))h { L(@"proposeMain %@", i); if (h) h(nil); }
- (void)notifyClickDidEndOnDesktopBackgroundWindow:(id)i { L(@"click on desktop %@", i); }
- (void)listener:(id)l didReceiveConnection:(id)c withContext:(id)ctx
{
	L(@"connection %@", c);
	static NSMutableArray *conns; if (!conns) conns = [NSMutableArray new]; [conns addObject:c];		/* the connection dies with its last reference: the client's port would be dead */
	cfgblk b = ^(id cfg) {
		[cfg performSelector:NSSelectorFromString(@"setInterface:") withObject:[NSClassFromString(@"WMXPCInterface") performSelector:NSSelectorFromString(@"serviceInterface")]];
		[cfg performSelector:NSSelectorFromString(@"setInterfaceTarget:") withObject:self];
		[cfg performSelector:NSSelectorFromString(@"setTargetQueue:") withObject:dispatch_get_main_queue()];
		[cfg performSelector:NSSelectorFromString(@"setInvalidationHandler:") withObject:^{ L(@"connection invalidated"); }];
	};
	[c performSelector:NSSelectorFromString(@"configureConnection:") withObject:b];
	[c performSelector:NSSelectorFromString(@"activate")];
}
@end

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "wait")) sleep(12);
	@autoreleasepool {
		dlopen("/System/Library/PrivateFrameworks/WindowManagement.framework/WindowManagement", RTLD_NOW);
		dlopen("/System/Library/PrivateFrameworks/BoardServices.framework/BoardServices", RTLD_NOW);
		Class wmi = NSClassFromString(@"WMXPCInterface"), lis = NSClassFromString(@"BSServiceConnectionListener");
		if (!wmi || !lis) { L(@"missing classes %p %p", wmi, lis); return 1; }
		static WMSrv *srv; srv = [WMSrv new];
		id dom = [wmi performSelector:NSSelectorFromString(@"bsServiceDomain")], svc = [wmi performSelector:NSSelectorFromString(@"bsAppServiceIdentifier")];
		L(@"domain %@ (%@) service %@ (%@) mach %@", dom, [dom class], svc, [svc class], [wmi performSelector:NSSelectorFromString(@"machServiceName")]);
		id conf = [[lis class] respondsToSelector:@selector(listenerWithConfiguration:handler:)] ? [NSClassFromString(@"BSServiceConnectionListenerConfiguration") performSelector:NSSelectorFromString(@"configurationWithDomain:service:") withObject:dom withObject:svc] : nil;
		L(@"configuration %@", conf);
		static id listener;
		void (^hand)(id, id) = ^(id l, id c) {
			L(@"handler %@ %@", [l class], [c class]);
			[srv listener:l didReceiveConnection:c withContext:nil];
		};
		listener = ((id (*)(id, SEL, id, id))objc_msgSend)(lis, NSSelectorFromString(@"listenerWithConfiguration:handler:"), conf, hand);
		L(@"listener made %@", listener);
		[listener performSelector:NSSelectorFromString(@"activate")];
		L(@"listening %@", listener);
		[[NSRunLoop mainRunLoop] run];
	}
	return 0;
}
