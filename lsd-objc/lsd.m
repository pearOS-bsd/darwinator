/* lsd-objc: the LaunchServices database services (com.apple.lsd.mapdb, com.apple.lsd.modifydb) as a program of ours.
 * It is a client of Foundation only: the protocols are found by name at run time, the calls answered by hand.
 * Built on a Mac with the Xcode tools (clang -fobjc-arc -framework Foundation lsd.m), run on pearOS where the libraries come from the cache.
 * Environment: LSD_STORE = the csstore file that the clients map (a database that was made by a Mac for the same applications). */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stdio.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
	@autoreleasepool {
		storePath = [NSString stringWithUTF8String:getenv("LSD_STORE") ?: "/var/db/lsd-store/store.csstore"];
		const char *svcs[][2] = { { "com.apple.lsd.mapdb", "_LSDReadProtocol" }, { "com.apple.lsd.modifydb", "_LSDModifyProtocol" } };
		NSMutableArray *keep = [NSMutableArray array];

		for (int i = 0; i < 2; i++) {
			Service *s = [Service new];
			s->proto = NSProtocolFromString([NSString stringWithUTF8String:svcs[i][1]]);
			s->name = [NSString stringWithUTF8String:svcs[i][0]];
			if (s->proto == nil) { logf_([NSString stringWithFormat:@"no protocol %s", svcs[i][1]]); continue; }
			NSXPCListener *l = [[NSXPCListener alloc] initWithMachServiceName:s->name];
			l.delegate = s;
			if (getenv("LSD_FRAMEWORK") || access("/var/db/lsd-store/framework", F_OK) == 0) {		/* the service classes of LaunchServices.framework itself serve the calls (the framework holds the code that lsd runs) */
				Class fc = NSClassFromString(i == 0 ? @"_LSDReadService" : @"_LSDModifyService");
				id fs = fc ? [[fc alloc] init] : nil;
				logf_([NSString stringWithFormat:@"framework service %@ for %@", fs, s->name]);
				if (fs) { l.delegate = fs; [keep addObject:fs]; }
			}
			[l resume];
			[keep addObject:s]; [keep addObject:l];
		}
		logf_(@"serving");
		[[NSRunLoop mainRunLoop] run];
	}
	return 0;
}
