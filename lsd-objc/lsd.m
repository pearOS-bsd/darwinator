/* lsd-objc: the LaunchServices database services (com.apple.lsd.mapdb, com.apple.lsd.modifydb) as a program of ours.
 * It is a client of Foundation only: the protocols are found by name at run time, the calls answered by hand.
 * Built on a Mac with the Xcode tools (clang -fobjc-arc -framework Foundation lsd.m), run on pearOS where the libraries come from the cache.
 * Environment: LSD_STORE = the csstore file that the clients map (a database that was made by a Mac for the same applications). */
#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>

extern const char *_Block_signature(void *);

static NSString *storePath;
static id (*FSNodeInit)(id, SEL, NSURL *, unsigned, NSError **);

static void logf_(NSString *s) { fprintf(stderr, "lsd-objc: %s\n", [s UTF8String]); }

@interface Service : NSObject <NSXPCListenerDelegate> {
@public
	Protocol *proto;
	NSString *name;
}
@end

@implementation Service
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	struct objc_method_description d = protocol_getMethodDescription(proto, sel, YES, YES);
	if (d.name == NULL)
		d = protocol_getMethodDescription(proto, sel, NO, YES);
	if (d.name != NULL)
		return [NSMethodSignature signatureWithObjCTypes:d.types];
	return [super methodSignatureForSelector:sel];
}
- (BOOL)respondsToSelector:(SEL)sel
{
	struct objc_method_description d = protocol_getMethodDescription(proto, sel, YES, YES);
	return d.name != NULL || [super respondsToSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)inv
{
	NSString *sel = NSStringFromSelector(inv.selector);
	NSMethodSignature *ms = inv.methodSignature;
	void *blk = NULL;

	for (NSUInteger i = 2; i < ms.numberOfArguments; i++)
		if (strcmp([ms getArgumentTypeAtIndex:i], "@?") == 0) { [inv getArgument:&blk atIndex:i]; }
	logf_([NSString stringWithFormat:@"%@: %@ (reply block signature %s)", name, sel, blk ? _Block_signature(blk) : "none"]);
	if (blk == NULL)
		return;
	/* the reply: the block is called by hand with the arguments of its signature */
	{
		const char *sg = _Block_signature(blk);
		int nobj = 0, other = 0;
		int inq = 0;

		sg += 0;
		for (const char *p = sg; *p; p++) { if (*p == '"') { inq = !inq; continue; } if (inq) continue; if (*p == '@') nobj++; else if (*p != 'v' && *p != '?' && *p != 'c') other++; }
		if ([sel isEqualToString:@"getServerStoreNonBlockingWithCompletionHandler:"] && storePath.length > 0) {
			Class FSNode = NSClassFromString(@"FSNode");
			NSError *e = nil;
			id node = [FSNode alloc];
			node = ((id(*)(id, SEL, NSURL *, unsigned, NSError **))objc_msgSend)(node, NSSelectorFromString(@"initWithURL:flags:error:"), [NSURL fileURLWithPath:storePath], 0, &e);
			logf_([NSString stringWithFormat:@"store node %@ (%@)", node, e]);
			id store = [NSClassFromString(@"_CSStore") alloc];
			store = ((id(*)(id, SEL, NSURL *, NSError **))objc_msgSend)(store, NSSelectorFromString(@"initWithContentsOfURL:error:"), [NSURL fileURLWithPath:storePath], &e);
			if (store) ((void(*)(id, SEL, char))objc_msgSend)(store, NSSelectorFromString(@"setReadOnly:"), 1);
			logf_([NSString stringWithFormat:@"store %@ (%@)", store, e]);
			((__bridge void (^)(id, id, id, char, id))blk)(store, node, nil, 0, nil);
		} else if ([sel isEqualToString:@"getServerStatusWithCompletionHandler:"]) {	/* the status of the server (a Mac's lsd says 256) */
			((__bridge void (^)(unsigned))blk)(256);
		} else if (strncmp(sg, "v@?cI@\"NSArray\"c@", 16) == 0) {	/* registerItemInfo:...: (success, identifier, the items, a flag, error): the registration is accepted, nothing is stored (the store is read only) */
			((__bridge void (^)(char, unsigned, id, char, id))blk)(0, 0, nil, 0, nil);
		} else if (strncmp(sg, "v@?I@", 5) == 0) {		/* (unsigned, error): registerContainerURL: the identifier of the container (1: the store is read only, nothing is really added), no error */
			((__bridge void (^)(unsigned, id))blk)(1, nil);
		} else if (other == 0 && nobj == 2)		/* v@? then one object (an @? counts one): (nil) */
			((__bridge void (^)(id))blk)(nil);
		else if (other == 0 && nobj == 3)
			((__bridge void (^)(id, id))blk)(nil, nil);
		else
			logf_(@"no answer for this signature");
	}
}
- (BOOL)listener:(NSXPCListener *)l shouldAcceptNewConnection:(NSXPCConnection *)c
{
	c.exportedInterface = [NSXPCInterface interfaceWithProtocol:proto];
	c.exportedObject = self;
	[c resume];
	logf_([NSString stringWithFormat:@"%@: a connection", name]);
	return YES;
}
@end

@interface Guard : NSObject <NSXPCListenerDelegate> { @public id inner; NSString *name; } @end
@implementation Guard
- (BOOL)listener:(NSXPCListener *)l shouldAcceptNewConnection:(NSXPCConnection *)c
{
	@try {
		BOOL r = [(id<NSXPCListenerDelegate>)inner listener:l shouldAcceptNewConnection:c];
		logf_([NSString stringWithFormat:@"%@: framework service accepted: %d", name, r]);
		return r;
	} @catch (NSException *e) {
		logf_([NSString stringWithFormat:@"%@: framework service threw %@: %@\n%@", name, e.name, e.reason, e.callStackSymbols]);
		return NO;
	}
}
@end

static void uncaught(NSException *e) { logf_([NSString stringWithFormat:@"UNCAUGHT %@: %@\n%@", e.name, e.reason, e.callStackSymbols]); }

int main(int argc, char **argv)
{
	if ((getenv("LSD_TRACE_EXC") || access("/var/db/lsd-store/trace", F_OK) == 0) && !getenv("OBJC_PRINT_EXCEPTION_THROW")) { setenv("OBJC_PRINT_EXCEPTION_THROW", "YES", 1); execv(argv[0], argv); }	/* the runtime reads the variable when it starts */
	NSSetUncaughtExceptionHandler(uncaught);
	@autoreleasepool {
		storePath = [NSString stringWithUTF8String:getenv("LSD_STORE") ?: "/var/db/lsd-store/store.csstore"];
		const char *svcs[][2] = { { "com.apple.lsd.mapdb", "_LSDReadProtocol" }, { "com.apple.lsd.modifydb", "_LSDModifyProtocol" } };
		NSMutableArray *keep = [NSMutableArray array];

		if (getenv("LSD_FRAMEWORK") || access("/var/db/lsd-store/framework", F_OK) == 0) {	/* the framework's service classes refuse to run outside the daemon: _LSAssertRunningInServer looks at this flag */
			id defs = ((id (*)(id, SEL))objc_msgSend)(NSClassFromString(@"_LSDefaults"), NSSelectorFromString(@"sharedInstance"));
			((void (*)(id, SEL, char))objc_msgSend)(defs, NSSelectorFromString(@"setServer:"), 1);
			logf_([NSString stringWithFormat:@"isServer %d", ((char (*)(id, SEL))objc_msgSend)(defs, NSSelectorFromString(@"isServer"))]);
			{	/* the server's database context: the daemon makes it before it serves (_LSServer_SetDatabaseExecutionContext is not exported: the address is the one of the cache of the Mac that pearOS maps -- 0x7ff803480cc4 in the cache, +0x1940c000 where the cache is mapped here) */
				void (*setctx)(id) = (void (*)(id))dlsym(RTLD_DEFAULT, "_LSServer_SetDatabaseExecutionContext");
				id ctx = ((id (*)(id, SEL))objc_msgSend)([NSClassFromString(@"LSDBExecutionContext") alloc], NSSelectorFromString(@"_init"));

				if (setctx == NULL) setctx = (void (*)(id))0x7ff81c88ccc4;
				logf_([NSString stringWithFormat:@"execution context %@, setter %p", ctx, setctx]);
				if (ctx) setctx(ctx);
				[keep addObject:ctx ?: (id)[NSNull null]];
			}
			if (access("/var/db/lsd-store/register", F_OK) == 0) {	/* register the bundles that are listed in the file (one path a line): in the server the calls of LaunchServices work on the database directly */
				NSString *lst = [NSString stringWithContentsOfFile:@"/var/db/lsd-store/register" encoding:NSUTF8StringEncoding error:nil];

				dispatch_semaphore_t done = dispatch_semaphore_create(0);
				NSArray *paths = [lst componentsSeparatedByString:@"\n"];
				id ctx2 = keep.lastObject;

				for (id o in keep) if ([o isKindOfClass:NSClassFromString(@"LSDBExecutionContext")]) ctx2 = o;
				void (^work)(id, NSError *) = ^(id c, NSError *err) {
					logf_([NSString stringWithFormat:@"write context %@ (%@)", c, err]);
					for (NSString *pth in paths) {
						if (pth.length == 0) continue;
						OSStatus rs = LSRegisterURL((__bridge CFURLRef)[NSURL fileURLWithPath:pth isDirectory:YES], true);
						logf_([NSString stringWithFormat:@"register %@ -> %d", pth, (int)rs]);
					}
					dispatch_semaphore_signal(done);
				};
				((void (*)(id, SEL, id))objc_msgSend)(ctx2, NSSelectorFromString(@"performAsyncWrite:"), work);
				dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
			}
			if (access("/var/db/lsd-store/seed", F_OK) == 0) {	/* seed the database: the builder of the framework scans the system for applications */
				dispatch_queue_t ioq = dispatch_queue_create("lsd-objc.io", DISPATCH_QUEUE_SERIAL);
				id b = ((id (*)(id, SEL, id))objc_msgSend)([NSClassFromString(@"LSDatabaseBuilder") alloc], NSSelectorFromString(@"initWithIOQueue:"), ioq);

				logf_([NSString stringWithFormat:@"builder %@", b]);
				@try {
					((void (*)(id, SEL, const char *))objc_msgSend)(b, NSSelectorFromString(@"createAndSeedLocalDatabase:"), "/var/db/lsd-store/local.csstore");
					logf_(@"seeded (createAndSeedLocalDatabase)");
				} @catch (NSException *e) { logf_([NSString stringWithFormat:@"seeding threw %@ %@", e.name, e.reason]); }
				[keep addObject:b];
			}
		}

		for (int i = 0; i < 2; i++) {
			Service *s = [Service new];
			s->proto = NSProtocolFromString([NSString stringWithUTF8String:svcs[i][1]]);
			s->name = [NSString stringWithUTF8String:svcs[i][0]];
			if (s->proto == nil) { logf_([NSString stringWithFormat:@"no protocol %s", svcs[i][1]]); continue; }
			NSXPCListener *l = [[NSXPCListener alloc] initWithMachServiceName:s->name];
			l.delegate = s;
			if (getenv("LSD_FRAMEWORK") || access("/var/db/lsd-store/framework", F_OK) == 0) {		/* the service classes of LaunchServices.framework itself serve the calls (the framework holds the code that lsd runs) */
				Class fc = NSClassFromString(i == 0 ? @"_LSDReadService" : @"_LSDModifyService");
				id fs = fc ? ((id (*)(id, SEL, id))objc_msgSend)([fc alloc], NSSelectorFromString(@"initWithXPCListener:"), l) : nil;
				logf_([NSString stringWithFormat:@"framework service %@ for %@", fs, s->name]);
				if (fs) { Guard *g = [Guard new]; g->inner = fs; g->name = s->name; l.delegate = g; [keep addObject:g]; [keep addObject:fs]; }
			}
			[l resume];
			[keep addObject:s]; [keep addObject:l];
		}
		logf_(@"serving");
		[[NSRunLoop mainRunLoop] run];
	}
	return 0;
}
