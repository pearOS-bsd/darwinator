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

@interface WMSrv : NSObject
@end
@implementation WMSrv
- (void)handleXPCWindowTransaction:(id)t completionHandler:(void (^)(void))h
{
	L(@"handleXPCWindowTransaction: %@", t);
	for (id a in [t performSelector:NSSelectorFromString(@"actions")]) describe_action(a);
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
