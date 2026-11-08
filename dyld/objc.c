/*
 * The Objective-C 2 ("modern runtime") metadata of a Mach-O image, turned into calls of the GNU runtime of the system
 * (libobjc2): the classes of the program are created with objc_allocateClassPair and friends, so they are real classes
 * of GNUstep, with GNUstep's Foundation and AppKit as their superclasses.  Done at load time, after bind, before the
 * initializers:   protocols, classes (ivars, methods, protocols), categories, selector references, class references,
 * super references, protocol references, constant strings (__cfstring), then the +load methods.
 * The messaging functions of Apple's runtime (objc_msgSend and its variants) are small trampolines over objc_msg_lookup.
 *
 * Only the 64 bit x86 layout is implemented (messaging trampolines in assembly).  The runtime functions are found with
 * dlsym, so machrun runs without libobjc2 when the program has no Objective-C.
 */
#include <dlfcn.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machrun.h"

typedef void *Class, *SEL, *IMP, *id, *Protocol, *Ivar;
typedef signed char BOOL;

/* ---- the runtime ---- */
static Class (*rt_allocateClassPair)(Class, const char *, size_t);
static void (*rt_registerClassPair)(Class);
static BOOL (*rt_addIvar)(Class, const char *, size_t, uint8_t, const char *);
static BOOL (*rt_addMethod)(Class, SEL, IMP, const char *);
static IMP (*rt_replaceMethod)(Class, SEL, IMP, const char *);
static BOOL (*rt_addProtocol)(Class, Protocol *);
static Class (*rt_getClass)(const char *);
static Class (*rt_object_getClass)(id);
static Class (*rt_getSuperclass)(Class);
static SEL (*rt_registerName)(const char *);
static const char *(*rt_selName)(SEL);
static Protocol *(*rt_allocateProtocol)(const char *);
static void (*rt_registerProtocol)(Protocol *);
static void (*rt_protocolAddMethod)(Protocol *, SEL, const char *, BOOL, BOOL);
static void (*rt_protocolAddProtocol)(Protocol *, Protocol *);
static Protocol *(*rt_getProtocol)(const char *);
static Ivar (*rt_getInstanceVariable)(Class, const char *);
static ptrdiff_t (*rt_ivarOffset)(Ivar);
void *(*machrun_objc_lookup)(id, SEL);
static void *(*rt_lookup_super)(void *, SEL);
static int rt_ready;

static int
rt_init(void)
{
	if (rt_ready) return (rt_ready > 0);
#define	R(var, name)	do { *(void **)&var = dlsym(RTLD_DEFAULT, name); if (var == NULL) { VLOG("the Objective-C runtime lacks %s\n", name); rt_ready = -1; return (0); } } while (0)
	R(rt_allocateClassPair, "objc_allocateClassPair"); R(rt_registerClassPair, "objc_registerClassPair"); R(rt_addIvar, "class_addIvar");
	R(rt_addMethod, "class_addMethod"); R(rt_replaceMethod, "class_replaceMethod"); R(rt_addProtocol, "class_addProtocol");
	R(rt_getClass, "objc_getClass"); R(rt_object_getClass, "object_getClass"); R(rt_getSuperclass, "class_getSuperclass");
	R(rt_registerName, "sel_registerName"); R(rt_selName, "sel_getName"); R(rt_allocateProtocol, "objc_allocateProtocol");
	R(rt_registerProtocol, "objc_registerProtocol"); R(rt_protocolAddMethod, "protocol_addMethodDescription");
	R(rt_protocolAddProtocol, "protocol_addProtocol"); R(rt_getProtocol, "objc_getProtocol");
	R(rt_getInstanceVariable, "class_getInstanceVariable"); R(rt_ivarOffset, "ivar_getOffset");
	R(machrun_objc_lookup, "objc_msg_lookup"); R(rt_lookup_super, "objc_msg_lookup_super");
#undef R
	rt_ready = 1;
	return (1);
}

/* ---- the structures of the file ---- */
struct o_class { struct o_class *isa, *superclass; void *cache, *vtable; uintptr_t bits; };
struct o_ro { uint32_t flags, instanceStart, instanceSize, reserved; const void *ivarLayout; const char *name;
    const void *baseMethods, *baseProtocols, *ivars, *weakIvarLayout, *baseProperties; };
struct o_list { uint32_t entsizeAndFlags, count; };
struct o_ivar { int32_t *offset; const char *name, *type; uint32_t alignment_raw, size; };
struct o_plist { uintptr_t count; void *list[]; };
struct o_proto { void *isa; const char *name; struct o_plist *protocols; const void *instMethods, *classMethods, *optInstMethods, *optClassMethods, *instProps; };
struct o_cat { const char *name; struct o_class *cls; const void *instMethods, *classMethods; struct o_plist *protocols; const void *instProps; };

#define	RO(c)	((struct o_ro *)((c)->bits & ~(uintptr_t)7))
#define	METHOD_SMALL	0x80000000u

/* calls cb(name, types, imp) for each method of a method list (big and small formats) */
static void
each_method(const void *list, void (*cb)(const char *, const char *, void *, void *), void *arg)
{
	const struct o_list *l = list; uint32_t i, entsize;
	if (l == NULL) return;
	entsize = l->entsizeAndFlags & ~3u & ~METHOD_SMALL & ~0x40000000u;
	for (i = 0; i < l->count; i++) {
		const uint8_t *e = (const uint8_t *)(l + 1) + (size_t)i * entsize;
		if (l->entsizeAndFlags & METHOD_SMALL) {
			const int32_t *m = (const int32_t *)e;
			const char *nm = (const char *)((const uint8_t *)&m[0] + m[0]);
			if (!(l->entsizeAndFlags & 0x40000000u)) nm = *(const char *const *)nm;	/* a reference to the selector */
			cb(nm, (const char *)((const uint8_t *)&m[1] + m[1]), (void *)((const uint8_t *)&m[2] + m[2]), arg);
		} else {
			void *const *m = (void *const *)e;
			cb((const char *)m[0], (const char *)m[1], m[2], arg);
		}
	}
}

/* ---- own classes and protocols ---- */
#define	MAXCLS	4096
static struct { struct o_class *c; Class k; } clsmap[MAXCLS]; static int nclsmap;
static struct { struct o_proto *p; Protocol *k; } promap[512]; static int npromap;

static Class own_class(void *p) { int i; for (i = 0; i < nclsmap; i++) if (clsmap[i].c == p) return (clsmap[i].k); return (NULL); }
static Class to_class(void *p) { Class k = own_class(p); return (k ? k : (Class)p); }
static Protocol *to_proto(void *p) { int i; for (i = 0; i < npromap; i++) if (promap[i].p == p) return (promap[i].k); return ((Protocol *)p); }

struct sel_add { Class cls; int replace; };
static void
add_method_cb(const char *nm, const char *types, void *imp, void *arg)
{
	struct sel_add *a = arg; SEL sel = rt_registerName(nm);
	if (a->replace) rt_replaceMethod(a->cls, sel, imp, types); else rt_addMethod(a->cls, sel, imp, types);
}
static void add_methods(Class cls, const void *list, int replace) { struct sel_add a = { cls, replace }; each_method(list, add_method_cb, &a); }
static void
add_protocols(Class cls, struct o_plist *pl)
{
	uintptr_t i;
	if (pl == NULL) return;
	for (i = 0; i < pl->count; i++) { Protocol *p = to_proto(pl->list[i]); if (p) rt_addProtocol(cls, p); }
}

static void
proto_method_cb(const char *nm, const char *types, void *imp, void *arg)
{
	void **a = arg; rt_protocolAddMethod((Protocol *)a[0], rt_registerName(nm), types ? types : "", (BOOL)(intptr_t)a[1], (BOOL)(intptr_t)a[2]);
}
static void
make_protocol(struct o_proto *p)
{
	Protocol *k = rt_getProtocol(p->name); void *a[3];
	if (k == NULL) {
		k = rt_allocateProtocol(p->name);
		if (k == NULL) return;
		a[0] = k;
		a[1] = (void *)1; a[2] = (void *)1; each_method(p->instMethods, proto_method_cb, a);
		a[1] = (void *)1; a[2] = (void *)0; each_method(p->classMethods, proto_method_cb, a);
		a[1] = (void *)0; a[2] = (void *)1; each_method(p->optInstMethods, proto_method_cb, a);
		a[1] = (void *)0; a[2] = (void *)0; each_method(p->optClassMethods, proto_method_cb, a);
		promap[npromap].p = p; promap[npromap++].k = k;	/* mapped before the inherited ones are added: they may refer back */
		return;
	}
	promap[npromap].p = p; promap[npromap++].k = k;
}
static void
finish_protocol(struct o_proto *p)
{
	Protocol *k = to_proto(p); uintptr_t i;
	if (k == NULL || p->protocols == NULL) return;
	for (i = 0; i < p->protocols->count; i++) { Protocol *s = to_proto(p->protocols->list[i]); if (s && s != k) rt_protocolAddProtocol(k, s); }
	rt_registerProtocol(k);
}

int objc_is_own(void *p);
static Class
build_class(struct o_class *c)
{
	struct o_ro *ro; Class k, sup = NULL; const struct o_list *iv; uint32_t i;
	if ((k = own_class(c)) != NULL) return (k);
	ro = RO(c);
	if (c->superclass)
		sup = objc_is_own(c->superclass) ? build_class(c->superclass) : (Class)c->superclass;
	if (rt_getClass(ro->name) != NULL) { VLOG("class %s exists already: kept\n", ro->name); k = rt_getClass(ro->name); }
	else {
		k = rt_allocateClassPair(sup, ro->name, 0);
		if (k == NULL) { VLOG("cannot create class %s\n", ro->name); return (NULL); }
		iv = ro->ivars;
		if (iv) for (i = 0; i < iv->count; i++) {
			const struct o_ivar *v = (const struct o_ivar *)((const uint8_t *)(iv + 1) + (size_t)i * (iv->entsizeAndFlags & ~3u));
			uint8_t al = v->alignment_raw == ~0u ? 3 : (uint8_t)v->alignment_raw;
			rt_addIvar(k, v->name, v->size, al, v->type);
		}
		rt_registerClassPair(k);
		if (iv) for (i = 0; i < iv->count; i++) {		/* the program's ivar offset variables get the offsets of this runtime */
			const struct o_ivar *v = (const struct o_ivar *)((const uint8_t *)(iv + 1) + (size_t)i * (iv->entsizeAndFlags & ~3u));
			Ivar r = rt_getInstanceVariable(k, v->name);
			if (r && v->offset) *v->offset = (int32_t)rt_ivarOffset(r);
		}
	}
	add_methods(k, ro->baseMethods, 0);
	if (c->isa) add_methods(rt_object_getClass((id)k), RO(c->isa)->baseMethods, 0);
	add_protocols(k, (struct o_plist *)ro->baseProtocols);
	clsmap[nclsmap].c = c; clsmap[nclsmap++].k = k;
	return (k);
}

static struct o_class **g_classlist; static size_t g_nclasses;
int objc_is_own(void *p) { size_t i; for (i = 0; i < g_nclasses; i++) if (g_classlist[i] == p) return (1); return (0); }

/* ---- constant strings: the 32 byte CFString constants become instances of a NSString subclass that reads them in place ---- */
static uint64_t cfs_len(id s) { return (((uint64_t *)s)[3]); }
static uint16_t
cfs_charAt(id s, SEL _cmd, uint64_t i)
{
	uint64_t *w = s; uint32_t flags = (uint32_t)w[1];
	if (i >= w[3]) return (0);
	return ((flags & 0x10) ? ((const uint16_t *)w[2])[i] : ((const unsigned char *)w[2])[i]);
}
static id cfs_self(id s) { return (s); }
static void cfs_void(id s) { }
static uint64_t cfs_rc(id s) { return (~(uint64_t)0 >> 1); }
static id cfs_copy(id s, SEL _cmd, void *zone) { return (s); }
static uint64_t cfs_lengthm(id s, SEL _cmd) { return (cfs_len(s)); }

static Class cfs_class;
static void
setup_cfstring_class(void)
{
	Class ns = rt_getClass("NSString"), meta;
	if (ns == NULL) { VLOG("no NSString: constant strings are left as they are\n"); return; }
	cfs_class = rt_allocateClassPair(ns, "MachoConstantString", 0);
	if (cfs_class == NULL) { cfs_class = rt_getClass("MachoConstantString"); return; }
	rt_addMethod(cfs_class, rt_registerName("length"), (IMP)cfs_lengthm, "Q16@0:8");
	rt_addMethod(cfs_class, rt_registerName("characterAtIndex:"), (IMP)cfs_charAt, "S24@0:8Q16");
	rt_addMethod(cfs_class, rt_registerName("retain"), (IMP)cfs_self, "@16@0:8");
	rt_addMethod(cfs_class, rt_registerName("autorelease"), (IMP)cfs_self, "@16@0:8");
	rt_addMethod(cfs_class, rt_registerName("release"), (IMP)cfs_void, "v16@0:8");
	rt_addMethod(cfs_class, rt_registerName("dealloc"), (IMP)cfs_void, "v16@0:8");
	rt_addMethod(cfs_class, rt_registerName("retainCount"), (IMP)cfs_rc, "Q16@0:8");
	rt_addMethod(cfs_class, rt_registerName("copyWithZone:"), (IMP)cfs_copy, "@24@0:8^{_NSZone=}16");
	(void)meta;
	rt_registerClassPair(cfs_class);
}

/* ---- +load, called after the segments have their final protections (the program's code runs) ---- */
static struct o_class **g_nlcls, **g_cl; static size_t g_nnlcls, g_ncl;
static IMP
find_load(struct o_class *cc)
{
	const struct o_list *ml = cc->isa ? RO(cc->isa)->baseMethods : NULL; uint32_t m, es;
	if (ml == NULL || (ml->entsizeAndFlags & METHOD_SMALL)) return (NULL);
	es = ml->entsizeAndFlags & ~3u;
	for (m = 0; m < ml->count; m++) {
		void *const *e = (void *const *)((const uint8_t *)(ml + 1) + (size_t)m * es);
		if (strcmp((const char *)e[0], "load") == 0) return (e[2]);
	}
	return (NULL);
}
void
objc_call_loads(void)
{
	static char done[MAXCLS]; SEL load; size_t i, j;
	if (g_nnlcls == 0 || !rt_ready) return;
	load = rt_registerName("load");
	for (i = 0; i < g_nnlcls; i++) {
		struct o_class *chain[32], *c = g_nlcls[i]; int depth = 0;
		while (c && objc_is_own(c) && depth < 32) { chain[depth++] = c; c = c->superclass; }
		while (depth--) {
			struct o_class *cc = chain[depth]; Class k = own_class(cc); IMP imp;
			for (j = 0; j < g_ncl; j++) if (g_cl[j] == cc) break;
			if (j == g_ncl || done[j] || k == NULL) continue;
			done[j] = 1;
			if ((imp = find_load(cc)) != NULL) { VLOG("+load of %s\n", RO(cc)->name); ((void (*)(id, SEL))imp)((id)k, load); }
		}
	}
}

/* ---- classes of Interface Builder's archives that GNUstep does not have ---- */
static id msg0(id o, const char *sel) { SEL s = rt_registerName(sel); return (((id (*)(id, SEL))machrun_objc_lookup(o, s))(o, s)); }
static id msg1(id o, const char *sel, id a) { SEL s = rt_registerName(sel); return (((id (*)(id, SEL, id))machrun_objc_lookup(o, s))(o, s, a)); }
static id
nsstr(const char *c)
{
	Class cls = rt_getClass("NSString"); SEL s = rt_registerName("stringWithUTF8String:");
	return (((id (*)(id, SEL, const char *))machrun_objc_lookup(cls, s))(cls, s, c));
}
/* NSLocalizableString (keys NS.string, NSDev, NSKey): the nib stores the text of a label with its localization key; decoding gives the string itself */
static id
ib_localizable_init(id self, SEL _cmd, id coder)
{
	id str = msg1(coder, "decodeObjectForKey:", nsstr("NS.string"));
	if (str == NULL) str = msg1(coder, "decodeObjectForKey:", nsstr("NSDev"));
	if (str == NULL) str = nsstr("");
	msg0(str, "retain");
	msg0(self, "release");
	return (str);
}
static void
ib_register_classes(void)
{
	Class ns = rt_getClass("NSObject"), k;
	if (ns == NULL || rt_getClass("NSLocalizableString") != NULL) return;
	k = rt_allocateClassPair(ns, "NSLocalizableString", 0);
	if (k == NULL) return;
	rt_addMethod(k, rt_registerName("initWithCoder:"), (IMP)ib_localizable_init, "@24@0:8@16");
	rt_registerClassPair(k);
	VLOG("objc: class NSLocalizableString (for nibs)\n");
}

/* ---- the entry point ---- */
typedef void *(*find_fn)(const char *sect, size_t *size);

int
objc_load_image(find_fn find, intptr_t slide)
{
	size_t n, i; void **p; struct o_class **cl; struct o_cat **cats; struct o_proto **prs; uintptr_t *refs;
	size_t ncl = 0, ncat = 0, npr = 0;

	cl = find("__objc_classlist", &n); ncl = n / sizeof(void *);
	cats = find("__objc_catlist", &n); ncat = n / sizeof(void *);
	prs = find("__objc_protolist", &n); npr = n / sizeof(void *);
	if (ncl + ncat + npr == 0 && find("__cfstring", &n) == NULL) return (0);
	if (!rt_init()) { warnx("the program has Objective-C metadata but the Objective-C runtime (libobjc2) could not be used"); return (-1); }
	VLOG("objc: %zu classes, %zu categories, %zu protocols\n", ncl, ncat, npr);
	ib_register_classes();

	g_classlist = cl; g_nclasses = ncl;
	for (i = 0; i < npr; i++) make_protocol(prs[i]);
	for (i = 0; i < npr; i++) finish_protocol(prs[i]);
	for (i = 0; i < ncl; i++) build_class(cl[i]);
	for (i = 0; i < ncat; i++) {
		struct o_cat *c = cats[i]; Class k = to_class(c->cls);
		if (k == NULL) continue;
		add_methods(k, c->instMethods, 1);
		add_methods(rt_object_getClass((id)k), c->classMethods, 1);
		add_protocols(k, c->protocols);
	}
	/* references: selectors become registered selectors, class and protocol references the runtime's objects */
	if ((refs = find("__objc_selrefs", &n))) for (i = 0; i < n / sizeof(void *); i++) refs[i] = (uintptr_t)rt_registerName((const char *)refs[i]);
	if ((refs = find("__objc_classrefs", &n))) for (i = 0; i < n / sizeof(void *); i++) refs[i] = (uintptr_t)to_class((void *)refs[i]);
	if ((refs = find("__objc_superrefs", &n))) for (i = 0; i < n / sizeof(void *); i++) refs[i] = (uintptr_t)to_class((void *)refs[i]);
	if ((refs = find("__objc_protorefs", &n))) for (i = 0; i < n / sizeof(void *); i++) refs[i] = (uintptr_t)to_proto((void *)refs[i]);
	/* constant strings */
	if ((p = find("__cfstring", &n)) != NULL) {
		setup_cfstring_class();
		if (cfs_class) for (i = 0; i < n / 32; i++) ((void **)p)[i * 4] = cfs_class;
		VLOG("objc: %zu constant strings\n", n / 32);
	}
	g_nlcls = find("__objc_nlclslist", &n); g_nnlcls = n / sizeof(void *); g_cl = cl; g_ncl = ncl;
	return (1);
}

/* ---- what the program binds to ---- */
extern void machrun_objc_msgSend(void), machrun_objc_msgSend_stret(void), machrun_objc_msgSendSuper2(void),
    machrun_objc_msgSendSuper2_stret(void), machrun_objc_msgSendSuper(void), machrun_objc_msgSendSuper_stret(void);
static char zero16[64];

struct pair { void *imp; void *recv; };
struct o_super { id receiver; Class cls; };
struct pair machrun_super2(struct o_super *s, SEL sel)
{
	struct o_super t = { s->receiver, rt_getSuperclass(s->cls) };
	return ((struct pair){ rt_lookup_super(&t, sel), s->receiver });
}
struct pair machrun_super1(struct o_super *s, SEL sel)
{
	struct o_super t = { s->receiver, s->cls };
	return ((struct pair){ rt_lookup_super(&t, sel), s->receiver });
}
struct pair (*machrun_super2_p)(struct o_super *, SEL) = machrun_super2;
struct pair (*machrun_super1_p)(struct o_super *, SEL) = machrun_super1;

void *
objc_shim_lookup(const char *name)
{
	if (strcmp(name, "_objc_msgSend") == 0 || strcmp(name, "_objc_msgSend_fpret") == 0) return ((void *)machrun_objc_msgSend);
	if (strcmp(name, "_objc_msgSend_stret") == 0) return ((void *)machrun_objc_msgSend_stret);
	if (strcmp(name, "_objc_msgSendSuper2") == 0) return ((void *)machrun_objc_msgSendSuper2);
	if (strcmp(name, "_objc_msgSendSuper2_stret") == 0) return ((void *)machrun_objc_msgSendSuper2_stret);
	if (strcmp(name, "_objc_msgSendSuper") == 0) return ((void *)machrun_objc_msgSendSuper);
	if (strcmp(name, "_objc_msgSendSuper_stret") == 0) return ((void *)machrun_objc_msgSendSuper_stret);
	if (strcmp(name, "__objc_empty_cache") == 0 || strcmp(name, "__objc_empty_vtable") == 0 || strcmp(name, "___CFConstantStringClassReference") == 0
	    || strcmp(name, "_OBJC_EHTYPE_$_NSException") == 0 || strcmp(name, "__objc_ehtype_vtable") == 0) return (zero16);
	if (strncmp(name, "_OBJC_CLASS_$_", 14) == 0 && rt_init()) {
		Class k = rt_getClass(name + 14);
		if (k == NULL) {					/* a class that the system does not have: an empty placeholder, so that the program starts */
			Class ns = rt_getClass("NSObject");
			k = rt_allocateClassPair(ns, name + 14, 0);
			if (k) { rt_registerClassPair(k); VLOG("placeholder class %s\n", name + 14); }
		}
		return (k);
	}
	if (strncmp(name, "_OBJC_METACLASS_$_", 18) == 0 && rt_init()) {
		Class k = rt_getClass(name + 18);
		return (k ? rt_object_getClass((id)k) : zero16);
	}
	if (strncmp(name, "__OBJC_PROTOCOL_$_", 18) == 0 && rt_init()) return (rt_getProtocol(name + 18));
	if (strcmp(name, "_NSZeroRect") == 0 || strcmp(name, "_NSZeroPoint") == 0 || strcmp(name, "_NSZeroSize") == 0
	    || strcmp(name, "_kCFAllocatorDefault") == 0 || strcmp(name, "_kCFAllocatorNull") == 0) return (zero16);
	if (strcmp(name, "___objc_personality_v0") == 0) return (dlsym(RTLD_DEFAULT, "__gnustep_objc_personality_v0"));
	return (NULL);
}

/* ---- the messaging trampolines (x86-64): look the method up with the GNU runtime, then jump to it with the registers as they were ---- */
#if defined(__x86_64__)
#define	SAVE_REGS \
	"pushq %rbp\n\tmovq %rsp,%rbp\n\tsubq $0xC0,%rsp\n\t" \
	"movq %rdi,0x00(%rsp)\n\tmovq %rsi,0x08(%rsp)\n\tmovq %rdx,0x10(%rsp)\n\tmovq %rcx,0x18(%rsp)\n\tmovq %r8,0x20(%rsp)\n\tmovq %r9,0x28(%rsp)\n\tmovq %rax,0x30(%rsp)\n\t" \
	"movdqu %xmm0,0x40(%rsp)\n\tmovdqu %xmm1,0x50(%rsp)\n\tmovdqu %xmm2,0x60(%rsp)\n\tmovdqu %xmm3,0x70(%rsp)\n\t" \
	"movdqu %xmm4,0x80(%rsp)\n\tmovdqu %xmm5,0x90(%rsp)\n\tmovdqu %xmm6,0xA0(%rsp)\n\tmovdqu %xmm7,0xB0(%rsp)\n\t"
#define	RESTORE_REGS \
	"movq 0x00(%rsp),%rdi\n\tmovq 0x08(%rsp),%rsi\n\tmovq 0x10(%rsp),%rdx\n\tmovq 0x18(%rsp),%rcx\n\tmovq 0x20(%rsp),%r8\n\tmovq 0x28(%rsp),%r9\n\tmovq 0x30(%rsp),%rax\n\t" \
	"movdqu 0x40(%rsp),%xmm0\n\tmovdqu 0x50(%rsp),%xmm1\n\tmovdqu 0x60(%rsp),%xmm2\n\tmovdqu 0x70(%rsp),%xmm3\n\t" \
	"movdqu 0x80(%rsp),%xmm4\n\tmovdqu 0x90(%rsp),%xmm5\n\tmovdqu 0xA0(%rsp),%xmm6\n\tmovdqu 0xB0(%rsp),%xmm7\n\t"

__asm__(
".text\n"
".globl machrun_objc_msgSend\n.type machrun_objc_msgSend,@function\nmachrun_objc_msgSend:\n\t"
"testq %rdi,%rdi\n\tjz 1f\n\t" SAVE_REGS "call *machrun_objc_lookup(%rip)\n\tmovq %rax,%r11\n\t" RESTORE_REGS "leave\n\tjmp *%r11\n"
"1:\txorl %eax,%eax\n\txorl %edx,%edx\n\txorps %xmm0,%xmm0\n\txorps %xmm1,%xmm1\n\tret\n"

".globl machrun_objc_msgSend_stret\n.type machrun_objc_msgSend_stret,@function\nmachrun_objc_msgSend_stret:\n\t"
"testq %rsi,%rsi\n\tjz 2f\n\t" SAVE_REGS "movq %rsi,%rdi\n\tmovq %rdx,%rsi\n\tcall *machrun_objc_lookup(%rip)\n\tmovq %rax,%r11\n\t" RESTORE_REGS "leave\n\tjmp *%r11\n"
"2:\tmovq %rdi,%rax\n\tret\n"

".globl machrun_objc_msgSendSuper2\n.type machrun_objc_msgSendSuper2,@function\nmachrun_objc_msgSendSuper2:\n\t"
SAVE_REGS "call *machrun_super2_p(%rip)\n\tmovq %rax,%r11\n\tmovq %rdx,%r10\n\t" RESTORE_REGS "movq %r10,%rdi\n\tleave\n\tjmp *%r11\n"

".globl machrun_objc_msgSendSuper\n.type machrun_objc_msgSendSuper,@function\nmachrun_objc_msgSendSuper:\n\t"
SAVE_REGS "call *machrun_super1_p(%rip)\n\tmovq %rax,%r11\n\tmovq %rdx,%r10\n\t" RESTORE_REGS "movq %r10,%rdi\n\tleave\n\tjmp *%r11\n"

".globl machrun_objc_msgSendSuper2_stret\n.type machrun_objc_msgSendSuper2_stret,@function\nmachrun_objc_msgSendSuper2_stret:\n\t"
SAVE_REGS "movq %rsi,%rdi\n\tmovq %rdx,%rsi\n\tcall *machrun_super2_p(%rip)\n\tmovq %rax,%r11\n\tmovq %rdx,%r10\n\t" RESTORE_REGS "movq %r10,%rsi\n\tleave\n\tjmp *%r11\n"

".globl machrun_objc_msgSendSuper_stret\n.type machrun_objc_msgSendSuper_stret,@function\nmachrun_objc_msgSendSuper_stret:\n\t"
SAVE_REGS "movq %rsi,%rdi\n\tmovq %rdx,%rsi\n\tcall *machrun_super1_p(%rip)\n\tmovq %rax,%r11\n\tmovq %rdx,%r10\n\t" RESTORE_REGS "movq %r10,%rsi\n\tleave\n\tjmp *%r11\n"
);
#else
void machrun_objc_msgSend(void) {}
void machrun_objc_msgSend_stret(void) {}
void machrun_objc_msgSendSuper2(void) {}
void machrun_objc_msgSendSuper2_stret(void) {}
void machrun_objc_msgSendSuper(void) {}
void machrun_objc_msgSendSuper_stret(void) {}
#endif

/* A constant that no library has (NSFileBusy, kSecClass, kUTTag...): a variable that holds an NSString with the name of the constant,
 * which is what such constants are on the Mac.  Only for names that look like constants: _NS<Upper>... and _k<Upper>... */
void *
objc_constant_fallback(const char *name)
{
	static struct { char *name; void **cell; } seen[256]; static int nseen; int i; Class cls; SEL sel; id str; void **cell;
	if (!((name[1] == 'N' && name[2] == 'S') || (name[1] == 'k' && name[2] >= 'A' && name[2] <= 'Z'))) return (NULL);
	if (name[1] == 'N' && !(name[3] >= 'A' && name[3] <= 'Z')) return (NULL);
	if (!rt_init() || (cls = rt_getClass("NSString")) == NULL) return (NULL);
	for (i = 0; i < nseen; i++) if (strcmp(seen[i].name, name) == 0) return (seen[i].cell);
	if (nseen == 256) return (NULL);
	sel = rt_registerName("stringWithUTF8String:");
	str = ((id (*)(id, SEL, const char *))machrun_objc_lookup(cls, sel))(cls, sel, name + 1);
	{ SEL r = rt_registerName("retain"); ((id (*)(id, SEL))machrun_objc_lookup(str, r))(str, r); }
	cell = malloc(sizeof(void *)); *cell = str;
	seen[nseen].name = strdup(name); seen[nseen++].cell = cell;
	VLOG("constant %s: made a string\n", name);
	return (cell);
}
