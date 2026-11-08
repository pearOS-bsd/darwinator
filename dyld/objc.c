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
#include <pthread.h>
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
uintptr_t machrun_cls_lo, machrun_cls_hi;
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

static FILE *symfile; static const char *sym_cls; static int sym_meta;
/* Apple's type encodings carry the frame size and the offsets of the arguments ("c40@0:8o^@16@24o^@32"), GNUstep's do not ("c@:o^@@o^@"); libobjc2 dispatches on the types too, so a
 * method with Apple's string is not found by a call from GNUstep.  The numbers outside of structure names and quoted names are taken out, and the qualifiers of arguments. */
static const char *
gnu_types(const char *t)
{
	size_t n = strlen(t); char *o = malloc(n + 1), *w = o; int depth = 0, quote = 0;
	for (; *t; t++) {
		if (*t == '"') quote = !quote;
		else if (!quote) { if (*t == '{' || *t == '(') depth++; else if (*t == '}' || *t == ')') depth--; }
		if (!quote && depth == 0 && ((*t >= '0' && *t <= '9') || strchr("nNoORV", *t) != NULL)) continue;	/* offsets, and the qualifiers in / inout / out / bycopy / byref / oneway: Apple's headers write (out id *) where GNUstep's do not */
		*w++ = *t;
	}
	*w = 0;
	return (o);
}

struct sel_add { Class cls; int replace; };
static void
add_method_cb(const char *nm, const char *types, void *imp, void *arg)
{
	struct sel_add *a = arg; SEL sel = rt_registerName(nm);
	types = gnu_types(types);
	if (symfile) fprintf(symfile, "%lx %c[%s %s]\n", (unsigned long)imp, sym_meta ? '+' : '-', sym_cls ? sym_cls : "?", nm);
	BOOL ok;
	if (a->replace) { rt_replaceMethod(a->cls, sel, imp, types); ok = 1; } else ok = rt_addMethod(a->cls, sel, imp, types);
	if (!ok || machrun_verbose > 1) {
		const char *(*cn)(Class) = dlsym(RTLD_DEFAULT, "class_getName");
		VLOG("objc: class_addMethod %s %s [%s] -> %d\n", cn ? cn(a->cls) : "?", nm, types, (int)ok);
	}
}
static void
add_methods(Class cls, const void *list, int replace)
{
	struct sel_add a = { cls, replace };
	const char *(*cn)(Class) = dlsym(RTLD_DEFAULT, "class_getName");
	sym_cls = cn ? cn(cls) : NULL; sym_meta = 0;
	if (sym_cls && symfile && rt_object_getClass((id)cls) == cls) sym_meta = 1;
	each_method(list, add_method_cb, &a);
}
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
	void **a = arg; rt_protocolAddMethod((Protocol *)a[0], rt_registerName(nm), types ? gnu_types(types) : "", (BOOL)(intptr_t)a[1], (BOOL)(intptr_t)a[2]);
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
	{ const char *(*cn)(Class) = dlsym(RTLD_DEFAULT, "class_getName"); VLOG("class %s: super %p %s%s\n", ro->name, (void *)c->superclass, c->superclass && !objc_is_own(c->superclass) && cn ? cn((Class)c->superclass) : "", c->superclass ? (objc_is_own(c->superclass) ? "(own)" : "") : " (NONE)"); }
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
	if (machrun_verbose && strcmp(ro->name, "KFormatter") == 0) {
		IMP (*gmi)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getMethodImplementation");
		SEL sel = rt_registerName("getObjectValue:forString:errorDescription:");
		const struct o_list *ml = ro->baseMethods; const void *const *e = (const void *const *)((const uint8_t *)(ml + 1) + 3 * 24);
		{	void *(*gim)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod"); IMP (*mgi)(void *) = dlsym(RTLD_DEFAULT, "method_getImplementation");
			const char *(*mgt)(void *) = dlsym(RTLD_DEFAULT, "method_getTypeEncoding"); BOOL (*rs)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_respondsToSelector");
			void *m = gim ? gim(k, sel) : NULL;
			fprintf(stderr, "objc: KFormatter: method %p imp %p types %s responds %d\n", m, m && mgi ? (void *)mgi(m) : NULL, m && mgt ? mgt(m) : "-", rs ? (int)rs(k, sel) & 0xff : -1); }
		{	id (*cci)(Class, size_t) = dlsym(RTLD_DEFAULT, "class_createInstance");
			if (cci) { id o = cci(k, 0); void *i1 = machrun_objc_lookup(o, sel); void *i2 = machrun_objc_lookup(o, rt_registerName("stringForObjectValue:")); fprintf(stderr, "objc: lookup on an instance: getObjectValue %p stringForObjectValue %p (program's 0x1000425c4 / 0x100042582)\n", i1, i2); }
		}
		fprintf(stderr, "objc: KFormatter %p super %p; getObjectValue: found %p, program's %p (%s)\n", (void *)k, (void *)rt_getSuperclass(k), gmi ? (void *)gmi(k, sel) : NULL, e[2], (const char *)e[0]);
	}
	clsmap[nclsmap].c = c; clsmap[nclsmap++].k = k;
	return (k);
}

/* the Objective-C metadata of every image that has some: the program and its dylibs */
struct oimg { struct o_class **cl; size_t ncl; struct o_class **nl; size_t nnl; char *done; };
static struct oimg oimgs[128]; static int noimgs;
int objc_is_own(void *p) { int k; size_t i; for (k = 0; k < noimgs; k++) for (i = 0; i < oimgs[k].ncl; i++) if (oimgs[k].cl[i] == p) return (1); return (0); }

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
	SEL load; size_t i, j; int k;
	if (noimgs == 0 || !rt_ready) return;
	load = rt_registerName("load");
	for (k = 0; k < noimgs; k++) {
		struct oimg *im = &oimgs[k];
		if (im->done == NULL) im->done = calloc(im->ncl + 1, 1);
		for (i = 0; i < im->nnl; i++) {
			struct o_class *chain[32], *c = im->nl[i]; int depth = 0;
			while (c && objc_is_own(c) && depth < 32) { chain[depth++] = c; c = c->superclass; }
			while (depth--) {
				struct o_class *cc = chain[depth]; Class kk = own_class(cc); IMP imp; int ki; size_t jj = 0; struct oimg *owner = NULL;
				for (ki = 0; ki < noimgs && owner == NULL; ki++) for (jj = 0; jj < oimgs[ki].ncl; jj++) if (oimgs[ki].cl[jj] == cc) { owner = &oimgs[ki]; break; }
				(void)j;
				if (owner == NULL || kk == NULL) continue;
				if (owner->done == NULL) owner->done = calloc(owner->ncl + 1, 1);
				if (owner->done[jj]) continue;
				owner->done[jj] = 1;
				if ((imp = find_load(cc)) != NULL) { VLOG("+load of %s\n", RO(cc)->name); ((void (*)(id, SEL))imp)((id)kk, load); }
			}
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

/* ---- fixes of GNUstep bugs that Apple's programs trip over ---- */
/* -[NSLayoutConstraint isEqual:] asks the other object for firstItem, secondItem ... without checking its class; NSIBObjectData puts
 * all the objects of a nib into a map table, which calls isEqual: with any object. */
static BOOL (*orig_constraint_isEqual)(id, SEL, id);
static BOOL
safe_constraint_isEqual(id self, SEL _cmd, id other)
{
	Class cc = rt_getClass("NSLayoutConstraint"); SEL k = rt_registerName("isKindOfClass:");
	if (self == other) return (1);
	if (other == NULL || !((BOOL (*)(id, SEL, Class))machrun_objc_lookup(other, k))(other, k, cc)) return (0);
	return (orig_constraint_isEqual(self, _cmd, other));
}
/* Apple's system font names (".AppleSystemUIFont", ".SFNS-Regular", "SFProText-Bold" ...) are not fonts of this system: fontWithName:size: answers nil and the text cannot be laid out.
 * Fall back to the system font (bold when the name says so). */
static id (*orig_fontWithName)(id, SEL, id, double);
static id
safe_fontWithName(id cls, SEL _cmd, id name, double size)
{
	id f = orig_fontWithName(cls, _cmd, name, size);
	const char *n; SEL s; int bold;
	if (f != NULL || name == NULL) return (f);
	n = ((const char *(*)(id, SEL))machrun_objc_lookup(name, rt_registerName("UTF8String")))(name, rt_registerName("UTF8String"));
	bold = n && (strstr(n, "Bold") || strstr(n, "Semibold") || strstr(n, "Heavy") || strstr(n, "Black"));
	s = rt_registerName(bold ? "boldSystemFontOfSize:" : "systemFontOfSize:");
	return (((id (*)(id, SEL, double))machrun_objc_lookup(cls, s))(cls, s, size > 0 ? size : 13.0));
}

/* On the Mac, text without a font attribute is laid out in Helvetica 12; GNUstep's typesetter raises "Glyph generation with no font".  Strings that miss the font get it before they are measured. */
typedef struct { double w, h; } MSize;
typedef struct { double x, y, w, h; } MRect;
typedef struct { unsigned long loc, len; } MRange;
static MRect (*orig_boundingRect)(id, SEL, MSize, unsigned long);
static id
with_default_font(id as)
{
	static id fontkey; id copy = NULL; unsigned long len, idx = 0; MRange r;
	SEL attr = rt_registerName("attribute:atIndex:effectiveRange:");
	if (fontkey == NULL) { void **p = dlsym(RTLD_DEFAULT, "NSFontAttributeName"); fontkey = p ? *p : NULL; if (fontkey == NULL) return (as); }
	len = (unsigned long)msg0(as, "length");
	while (idx < len) {
		id f = ((id (*)(id, SEL, id, unsigned long, MRange *))machrun_objc_lookup(as, attr))(as, attr, fontkey, idx, &r);
		Class cf = rt_getClass("NSFont"); SEL kind = rt_registerName("isKindOfClass:");
		if (f != NULL && !((BOOL (*)(id, SEL, Class))machrun_objc_lookup(f, kind))(f, kind, cf)) {
			/* something that is not a font in the font attribute (a font descriptor: a font that the system does not have): make a font from it, or the default one */
			Class cd = rt_getClass("NSFontDescriptor"); id nf = NULL;
			if (cd && ((BOOL (*)(id, SEL, Class))machrun_objc_lookup(f, kind))(f, kind, cd)) { SEL fd = rt_registerName("fontWithDescriptor:size:"); nf = ((id (*)(id, SEL, id, double))machrun_objc_lookup(cf, fd))(cf, fd, f, 0.0); }
			if (nf != NULL) {
				SEL add = rt_registerName("addAttribute:value:range:");
				if (copy == NULL) copy = msg0(msg0(as, "mutableCopy"), "autorelease");
				((void (*)(id, SEL, id, id, MRange))machrun_objc_lookup(copy, add))(copy, add, fontkey, nf, r);
				f = nf;
			} else f = NULL;
		}
		if (f == NULL) {
			SEL u = rt_registerName("userFontOfSize:"), add = rt_registerName("addAttribute:value:range:");
			id font = ((id (*)(id, SEL, double))machrun_objc_lookup(cf, u))(cf, u, 12.0);
			if (copy == NULL) copy = msg0(msg0(as, "mutableCopy"), "autorelease");
			((void (*)(id, SEL, id, id, MRange))machrun_objc_lookup(copy, add))(copy, add, fontkey, font, r);
		}
		if (r.len == 0) break;
		idx = r.loc + r.len;
	}
	return (copy ? copy : as);
}
static MRect
safe_boundingRect(id self, SEL _cmd, MSize size, unsigned long options)
{
	return (orig_boundingRect(with_default_font(self), _cmd, size, options));
}

/* MACHRUN_SHOWWINDOWS=1: one second after the application has launched, list its windows and order them front (a nib window that should be visible at launch is not shown by GNUstep) */
static void (*orig_finishLaunching)(id, SEL);
static void
machrun_show_windows(id app, SEL _cmd, id arg)
{
	id wins, del = msg0(app, "delegate"); unsigned long n, i; SEL at = rt_registerName("objectAtIndex:");
	if (del) {	/* what Apple's AppKit does for a launch without documents: ask the delegate, then let it open an untitled window */
		SEL should = rt_registerName("applicationShouldOpenUntitledFile:"), open = rt_registerName("applicationOpenUntitledFile:");
		SEL rsp = rt_registerName("respondsToSelector:"), reopen = rt_registerName("applicationShouldHandleReopen:hasVisibleWindows:");
		fprintf(stderr, "delegate %p responds: shouldOpenUntitled %d openUntitled %d reopen %d\n", del,
		    (int)(intptr_t)((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, should) & 0xff,
		    (int)(intptr_t)((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, open) & 0xff,
		    (int)(intptr_t)((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, reopen) & 0xff);
		if (((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, should)) {
			BOOL y = ((BOOL (*)(id, SEL, id))machrun_objc_lookup(del, should))(del, should, app);
			fprintf(stderr, "applicationShouldOpenUntitledFile: -> %d\n", (int)y & 0xff);
			if (y && ((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, open)) ((BOOL (*)(id, SEL, id))machrun_objc_lookup(del, open))(del, open, app);
		}
		if (((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, reopen)) {
			BOOL y = ((BOOL (*)(id, SEL, id, BOOL))machrun_objc_lookup(del, reopen))(del, reopen, app, 0);
			fprintf(stderr, "applicationShouldHandleReopen: -> %d\n", (int)y & 0xff);
		}
	}
	if (getenv("MACHRUN_ACTION")) {	/* MACHRUN_ACTION=openPreferences:  sends that action to the application, as a menu item would */
		SEL act = rt_registerName(getenv("MACHRUN_ACTION")), sa = rt_registerName("sendAction:to:from:");
		BOOL ok = ((BOOL (*)(id, SEL, SEL, id, id))machrun_objc_lookup(app, sa))(app, sa, act, NULL, NULL);
		fprintf(stderr, "action %s -> %d\n", getenv("MACHRUN_ACTION"), (int)ok & 0xff);
	}
	if (getenv("MACHRUN_OPENFILE") && del) {	/* MACHRUN_OPENFILE=/path/archive.zip: what a double click or a drop on the Dock does */
		const char *path = getenv("MACHRUN_OPENFILE"); id str = nsstr(path), arr, url;
		Class ca = rt_getClass("NSArray"), cu = rt_getClass("NSURL");
		SEL wo = rt_registerName("arrayWithObject:"), fu = rt_registerName("fileURLWithPath:"), of = rt_registerName("application:openFiles:"), ou = rt_registerName("application:openURLs:"), rsp = rt_registerName("respondsToSelector:");
		url = ((id (*)(id, SEL, id))machrun_objc_lookup(cu, fu))(cu, fu, str);
		if (((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, ou)) {
			arr = ((id (*)(id, SEL, id))machrun_objc_lookup(ca, wo))(ca, wo, url);
			((void (*)(id, SEL, id, id))machrun_objc_lookup(del, ou))(del, ou, app, arr); fprintf(stderr, "openURLs: %s\n", path);
		} else if (((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(del, rsp))(del, rsp, of)) {
			arr = ((id (*)(id, SEL, id))machrun_objc_lookup(ca, wo))(ca, wo, str);
			((void (*)(id, SEL, id, id))machrun_objc_lookup(del, of))(del, of, app, arr); fprintf(stderr, "openFiles: %s\n", path);
		} else fprintf(stderr, "the delegate does not open files\n");
	}
	wins = msg0(app, "windows"); n = (unsigned long)msg0(wins, "count");
	fprintf(stderr, "windows: %lu\n", n);
	for (i = 0; i < n; i++) {
		id w = ((id (*)(id, SEL, unsigned long))machrun_objc_lookup(wins, at))(wins, at, i);
		id t = msg0(w, "title"); const char *u = t ? ((const char *(*)(id, SEL))machrun_objc_lookup(t, rt_registerName("UTF8String")))(t, rt_registerName("UTF8String")) : "";
		Class k = rt_object_getClass(w); const char *(*cn)(Class) = dlsym(RTLD_DEFAULT, "class_getName");
		fprintf(stderr, "window %lu: %s '%s' visible %d\n", i, cn ? cn(k) : "?", u ? u : "", (int)(intptr_t)msg0(w, "isVisible") & 0xff);
		if (!getenv("MACHRUN_ACTION") || msg0(w, "isVisible")) msg1(w, "makeKeyAndOrderFront:", NULL);
	}
}
/* MACHRUN_KVC=key.path (with MACHRUN_KVC_DELAY=seconds, 20 by default): prints [NSApp.delegate valueForKeyPath:] to stderr, for the tests of programs (what the application holds after it did something) */
static void
machrun_kvc(id app, SEL _cmd, id arg)
{
	id del = msg0(app, "delegate"), v, d; const char *path = getenv("MACHRUN_KVC"), *u;
	SEL vk = rt_registerName("valueForKeyPath:"), ds = rt_registerName("description"), us = rt_registerName("UTF8String");
	if (!del || !path) return;
	v = ((id (*)(id, SEL, id))machrun_objc_lookup(del, vk))(del, vk, nsstr(path));
	d = v ? ((id (*)(id, SEL))machrun_objc_lookup(v, ds))(v, ds) : NULL;
	u = d ? ((const char *(*)(id, SEL))machrun_objc_lookup(d, us))(d, us) : "(nil)";
	fprintf(stderr, "KVC %s = %s\nKVC end\n", path, u);
}
static volatile int trace_on = 1;	/* MACHRUN_TRACE_AFTER_INVOKE=1: the trace starts when MACHRUN_INVOKE sends its action */
/* MACHRUN_INVOKE="key.path action:" (with MACHRUN_INVOKE_DELAY, 30 by default): sends the action to that object of the application with a nil sender, as a click on a button would */
static void
machrun_invoke(id app, SEL _cmd, id arg)
{
	char *spec = getenv("MACHRUN_INVOKE") ? strdup(getenv("MACHRUN_INVOKE")) : NULL, *sp; id del = msg0(app, "delegate"), target; SEL vk = rt_registerName("valueForKeyPath:"), act;
	if (!spec || !del || (sp = strchr(spec, ' ')) == NULL) return;
	*sp++ = 0;
	target = ((id (*)(id, SEL, id))machrun_objc_lookup(del, vk))(del, vk, nsstr(spec));
	act = rt_registerName(sp);
	fprintf(stderr, "INVOKE %s %s -> target %p\n", spec, sp, (void *)target);
	trace_on = 1;
	if (target) ((void (*)(id, SEL, id))machrun_objc_lookup(target, act))(target, act, NULL);
	fprintf(stderr, "INVOKE done\n");
}
static void
safe_finishLaunching(id self, SEL _cmd)
{
	VLOG("objc: finishLaunching\n");
	orig_finishLaunching(self, _cmd);
	VLOG("objc: finishLaunching done\n");
	if (getenv("MACHRUN_INVOKE")) {
		SEL s = rt_registerName("machrunInvoke:"), p = rt_registerName("performSelector:withObject:afterDelay:");
		rt_addMethod(rt_object_getClass(self), s, (IMP)machrun_invoke, "v24@0:8@16");
		((void (*)(id, SEL, SEL, id, double))machrun_objc_lookup(self, p))(self, p, s, NULL, getenv("MACHRUN_INVOKE_DELAY") ? atof(getenv("MACHRUN_INVOKE_DELAY")) : 30.0);
	}
	if (getenv("MACHRUN_KVC")) {
		SEL s = rt_registerName("machrunKVC:"), p = rt_registerName("performSelector:withObject:afterDelay:");
		rt_addMethod(rt_object_getClass(self), s, (IMP)machrun_kvc, "v24@0:8@16");
		((void (*)(id, SEL, SEL, id, double))machrun_objc_lookup(self, p))(self, p, s, NULL, getenv("MACHRUN_KVC_DELAY") ? atof(getenv("MACHRUN_KVC_DELAY")) : 20.0);
	}
	if (getenv("MACHRUN_SHOWWINDOWS")) {
		SEL s = rt_registerName("machrunShowWindows:"), p = rt_registerName("performSelector:withObject:afterDelay:");
		Class k = rt_object_getClass(self);
		rt_addMethod(k, s, (IMP)machrun_show_windows, "v24@0:8@16");
		((void (*)(id, SEL, SEL, id, double))machrun_objc_lookup(self, p))(self, p, s, NULL, 1.0);
	}
}

static void
gnustep_fixes(void)
{
	{	Class app = rt_getClass("NSApplication");
		void *(*gim3)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
		IMP (*smi3)(void *, IMP) = dlsym(RTLD_DEFAULT, "method_setImplementation");
		void *m;
		if (app && gim3 && smi3 && orig_finishLaunching == NULL) {
			msg0((id)app, "self");
			m = gim3(app, rt_registerName("finishLaunching"));
			if (m) orig_finishLaunching = (void (*)(id, SEL))smi3(m, (IMP)safe_finishLaunching);
		}
	}
	{	Class na = rt_getClass("NSAttributedString");
		void *(*gim2)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
		IMP (*smi2)(void *, IMP) = dlsym(RTLD_DEFAULT, "method_setImplementation");
		void *m;
		if (na && gim2 && smi2) {
			msg0((id)na, "self");
			m = gim2(na, rt_registerName("boundingRectWithSize:options:"));
			VLOG("objc: NSAttributedString %p, boundingRectWithSize:options: method %p\n", (void *)na, m);
			if (m && orig_boundingRect == NULL) { orig_boundingRect = (MRect (*)(id, SEL, MSize, unsigned long))smi2(m, (IMP)safe_boundingRect); VLOG("objc: NSAttributedString boundingRectWithSize:options: adds the default font\n"); }
		}
	}
	{	Class nf = rt_getClass("NSFont");
		void *(*gcm)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getClassMethod");
		IMP (*smi)(void *, IMP) = dlsym(RTLD_DEFAULT, "method_setImplementation");
		if (nf && gcm && smi) {
			void *m; msg0((id)nf, "self");
			m = gcm(nf, rt_registerName("fontWithName:size:"));
			if (m && orig_fontWithName == NULL) { orig_fontWithName = (id (*)(id, SEL, id, double))smi(m, (IMP)safe_fontWithName); VLOG("objc: NSFont fontWithName:size: falls back to the system font\n"); }
		}
	}
	Class cc = rt_getClass("NSLayoutConstraint");
	if (cc == NULL || orig_constraint_isEqual != NULL) return;
	msg0((id)cc, "self");			/* a message resolves and initializes a lazily registered class before its methods are changed */
	{	void *(*gim)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
		IMP (*smi)(void *, IMP) = dlsym(RTLD_DEFAULT, "method_setImplementation");
		void *m = gim ? gim(cc, rt_registerName("isEqual:")) : NULL;
		if (m && smi) orig_constraint_isEqual = (BOOL (*)(id, SEL, id))smi(m, (IMP)safe_constraint_isEqual);
		else orig_constraint_isEqual = (BOOL (*)(id, SEL, id))rt_replaceMethod(cc, rt_registerName("isEqual:"), (IMP)safe_constraint_isEqual, "c24@0:8@16"); }
	{	IMP (*cgmi)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getMethodImplementation");
		VLOG("objc: fixed -[NSLayoutConstraint isEqual:] (class %p, original %p, now %p, ours %p)\n", (void *)cc, (void *)orig_constraint_isEqual,
		    cgmi ? (void *)cgmi(cc, rt_registerName("isEqual:")) : NULL, (void *)safe_constraint_isEqual); }
}

/* ---- Apple API that GNUstep lacks: methods added to its classes when they are missing (found by running real programs) ---- */
static id nsud_initWithSuiteName(id self, SEL _cmd, id suite) { return (msg0(self, "init")); }	/* the suite is the app group of the program: one set of defaults is enough here */

static __attribute__((unused)) BOOL compat_yes(id self, SEL _cmd) { return (1); }
static BOOL compat_yes1(id self, SEL _cmd, id a) { return (1); }
static long compat_zero(id self, SEL _cmd) { return (0); }
static long compat_zero1(id self, SEL _cmd, id a) { return (0); }
static __attribute__((unused)) id compat_self(id self, SEL _cmd) { return (self); }
static id compat_self1(id self, SEL _cmd, id a) { return (self); }
static __attribute__((unused)) id compat_nil(id self, SEL _cmd) { return (NULL); }
static id compat_nil1(id self, SEL _cmd, id a) { return (NULL); }
static id compat_description(id self, SEL _cmd) { return (msg0(self, "description")); }
static id compat_srgb(id self, SEL _cmd) { return (msg0(self, "genericRGBColorSpace")); }	/* +[NSColorSpace sRGBColorSpace] */

static struct { const char *cls, *sel, *types; IMP imp; int meta; } compat_methods[] = {
	{ "NSUserDefaults", "initWithSuiteName:", "@24@0:8@16", (IMP)nsud_initWithSuiteName, 0 },
	{ "NSApplication", "setActivationPolicy:", "c24@0:8q16", (IMP)compat_yes1, 0 },
	{ "NSApplication", "activationPolicy", "q16@0:8", (IMP)compat_zero, 0 },
	{ "NSView", "addTrackingArea:", "v24@0:8@16", (IMP)compat_zero1, 0 },
	{ "NSView", "removeTrackingArea:", "v24@0:8@16", (IMP)compat_zero1, 0 },
	{ "NSColorSpace", "sRGBColorSpace", "@16@0:8", (IMP)compat_srgb, 1 },
	{ "NSFontDescriptor", "fontDescriptorWithDesign:", "@24@0:8@16", (IMP)compat_self1, 0 },
	{ "NSWorkspace", "URLForApplicationToOpenContentType:", "@24@0:8@16", (IMP)compat_nil1, 0 },
	{ "NSObject", "debugDescription", "@16@0:8", (IMP)compat_description, 0 },
	/* window and view appearance of macOS 10.10 - 14 that only matters for the look: accepted and ignored */
	{ "NSWindow", "setStyleMask:", "v24@0:8Q16", (IMP)compat_zero1, 0 },
	{ "NSWindow", "setTitlebarAppearsTransparent:", "v20@0:8c16", (IMP)compat_zero1, 0 },
	{ "NSWindow", "titlebarAppearsTransparent", "c16@0:8", (IMP)compat_zero, 0 },
	{ "NSWindow", "setTitleVisibility:", "v24@0:8q16", (IMP)compat_zero1, 0 },
	{ "NSWindow", "titleVisibility", "q16@0:8", (IMP)compat_zero, 0 },
	{ "NSWindow", "setToolbarStyle:", "v24@0:8q16", (IMP)compat_zero1, 0 },
	{ "NSWindow", "setAppearance:", "v24@0:8@16", (IMP)compat_zero1, 0 },
	{ "NSWindow", "appearance", "@16@0:8", (IMP)compat_nil, 0 },
	{ "NSWindow", "setTabbingMode:", "v24@0:8q16", (IMP)compat_zero1, 0 },
	{ "NSWindow", "setAllowsAutomaticWindowTabbing:", "v20@0:8c16", (IMP)compat_zero1, 1 },
	{ "NSWindow", "setAnimationBehavior:", "v24@0:8q16", (IMP)compat_zero1, 0 },
	{ "NSWindow", "setMovableByWindowBackground:", "v20@0:8c16", (IMP)compat_zero1, 0 },
	{ "NSView", "setAppearance:", "v24@0:8@16", (IMP)compat_zero1, 0 },
	{ "NSView", "appearance", "@16@0:8", (IMP)compat_nil, 0 },
	{ "NSView", "setWantsLayer:", "v20@0:8c16", (IMP)compat_zero1, 0 },
	{ "NSView", "setLayerContentsRedrawPolicy:", "v24@0:8q16", (IMP)compat_zero1, 0 },
	{ "NSApplication", "setAppearance:", "v24@0:8@16", (IMP)compat_zero1, 0 },
	{ "NSApplication", "appearance", "@16@0:8", (IMP)compat_nil, 0 },
	{ "NSApplication", "effectiveAppearance", "@16@0:8", (IMP)compat_nil, 0 },
	{ "NSApplication", "setAutomaticCustomizeTouchBarMenuItemEnabled:", "v20@0:8c16", (IMP)compat_zero1, 1 },
};

static void
add_compat_methods(void)
{
	size_t i;
	for (i = 0; i < sizeof(compat_methods) / sizeof(compat_methods[0]); i++) {
		Class k = rt_getClass(compat_methods[i].cls);
		if (k == NULL) continue;
		msg0((id)k, "self");
		if (rt_addMethod(compat_methods[i].meta ? rt_object_getClass((id)k) : k, rt_registerName(compat_methods[i].sel), compat_methods[i].imp, compat_methods[i].types))
			VLOG("objc: added -[%s %s]\n", compat_methods[i].cls, compat_methods[i].sel);
	}
}

/* ---- lenient mode (MACHRUN_LENIENT=1): a message that nothing implements is a logged no-op returning 0 instead of an exception; for bring-up,
 * it lists in one run the Apple API that GNUstep lacks.  Selectors the frameworks themselves probe with are left alone. ---- */
static long lenient_nop(id self, SEL _cmd) { return (0); }
static int
lenient_skip(const char *n)
{
	static const char *const deny[] = { "forwardInvocation:", "methodSignatureForSelector:", "forwardingTargetForSelector:", "doesNotRecognizeSelector:",
	    "resolveInstanceMethod:", "resolveClassMethod:", "awakeAfterUsingCoder:", "replacementObjectForCoder:", "classForCoder", "classForKeyedArchiver",
	    "replacementObjectForKeyedArchiver:", "initialize", "load", "copyWithZone:", "mutableCopyWithZone:", "encodeWithCoder:", "initWithCoder:", NULL };
	int i;
	if (n[0] == '_') return (1);
	for (i = 0; deny[i]; i++) if (strcmp(n, deny[i]) == 0) return (1);
	return (0);
}
static IMP (*lenient_class_gmi)(Class, SEL);
static IMP (*lenient_orig_forward)(id, SEL);
static IMP
lenient_forward3(id obj, SEL sel)
{
	static char seen[1024][64]; static int nseen; int i; Class k; const char *n = rt_selName(sel), *(*cn)(Class) = dlsym(RTLD_DEFAULT, "class_getName");
	static IMP nsobj_fi; static Class nsobj;
	if (lenient_skip(n) || obj == NULL) return (lenient_orig_forward ? lenient_orig_forward(obj, sel) : NULL);
	if (nsobj == NULL) { nsobj = rt_getClass("NSObject"); nsobj_fi = lenient_class_gmi(nsobj, rt_registerName("forwardInvocation:")); }
	k = rt_object_getClass(obj);
	if (k == NULL || (cn && cn(k) == NULL)) { fprintf(stderr, "lenient: -[%p %s]: the receiver has no class (isa %p); object words %p %p %p %p; isa words %p %p %p %p\n", obj, n, k, ((void **)obj)[0], ((void **)obj)[1], ((void **)obj)[2], ((void **)obj)[3], ((void **)k)[0], ((void **)k)[1], ((void **)k)[2], ((void **)k)[3]); return (lenient_orig_forward ? lenient_orig_forward(obj, sel) : NULL); }
	if (lenient_class_gmi(k, rt_registerName("forwardInvocation:")) != nsobj_fi) return (lenient_orig_forward ? lenient_orig_forward(obj, sel) : NULL);	/* the class forwards messages itself (a proxy): leave it to GNUstep */
	for (i = 0; i < nseen; i++) if (strcmp(seen[i], n) == 0) goto quiet;
	if (nseen < 1024) { strlcpy(seen[nseen++], n, 64); fprintf(stderr, "lenient: stubbed %s%s %s\n", "-/+ [", cn ? cn(k) : "?", n); }
quiet:
	return ((IMP)lenient_nop);
}
static void
setup_lenient(void)
{
	IMP (**f3)(id, SEL) = dlsym(RTLD_DEFAULT, "__objc_msg_forward3"), (**f2)(id, SEL) = dlsym(RTLD_DEFAULT, "__objc_msg_forward2");
	lenient_class_gmi = dlsym(RTLD_DEFAULT, "class_getMethodImplementation");
	if ((f2 == NULL && f3 == NULL) || lenient_class_gmi == NULL) { VLOG("objc: no forwarding hooks in this runtime: lenient mode is not available\n"); return; }
	VLOG("objc: forwarding hooks: forward2 %p (%p), forward3 %p (%p)\n", (void *)f2, f2 ? (void *)*f2 : NULL, (void *)f3, f3 ? (void *)*f3 : NULL);
	if (f2 && *f2) { lenient_orig_forward = *f2; *f2 = lenient_forward3; }
	if (f3 && *f3) { if (lenient_orig_forward == NULL) lenient_orig_forward = *f3; *f3 = lenient_forward3; }
	else if (f3) *f3 = lenient_forward3;
	VLOG("objc: lenient mode: unknown messages are stubbed\n");
}

/* ---- Program-specific compatibility: a Mac application that was built for the sandbox of the Mac App Store asks its own "do I have access to this folder" before it touches a file
 * (security-scoped bookmarks, which this system does not have); outside of the sandbox every access is allowed. ---- */
static BOOL yes_any(id self, SEL _cmd) { return (1); }
static void nop_any(id self, SEL _cmd) { }
static IMP orig_setdest;
static void
dbg_setdest(id self, SEL _cmd, id v)
{
	IMP (*gimp)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getMethodImplementation"); Class (*gc)(id) = dlsym(RTLD_DEFAULT, "object_getClass");
	id (*desc)(id, SEL) = v ? (id (*)(id, SEL))gimp(gc(v), rt_registerName("description")) : NULL;
	const char *(*utf)(id, SEL) = v ? (const char *(*)(id, SEL))gimp(gc(desc(v, rt_registerName("description"))), rt_registerName("UTF8String")) : NULL;
	VLOG("objc: KTask setDestination: %p %s\n", (void *)v, v ? utf(desc(v, rt_registerName("description")), rt_registerName("UTF8String")) : "(nil)");
	if (v == NULL) { /* the destination is derived from a format that this runtime cannot evaluate: use the folder of the archive */
		id f = ((id (*)(id, SEL))gimp(gc(self), rt_registerName("file")))(self, rt_registerName("file"));
		v = f ? ((id (*)(id, SEL))gimp(gc(f), rt_registerName("stringByDeletingLastPathComponent")))(f, rt_registerName("stringByDeletingLastPathComponent")) : NULL;
		VLOG("objc: KTask setDestination: nil replaced by the folder of the file %p\n", (void *)v);
	}
	((void (*)(id, SEL, id))orig_setdest)(self, _cmd, v);
}
static void
program_patches(void)
{
	{ void *(*g)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod"); IMP (*sm)(void *, IMP) = dlsym(RTLD_DEFAULT, "method_setImplementation"); Class k = rt_getClass("KTask");
	  void *m = k ? g(k, rt_registerName("setDestination:")) : NULL; if (m) orig_setdest = sm(m, (IMP)dbg_setdest); }
	static const struct { const char *cls, *sel; } p[] = {
		{ "sandboxHelpers", "hasAccess:" }, { "sandboxHelpers", "hasAccess:askIfNeeded:" }, { "sandboxHelpers", "hasFullAccess" },
		{ "sandboxHelpers", "hasHomeAccess" }, { "sandboxHelpers", "hasVolumesAccess" }, { "sandboxHelpers", "hasReadAccess:" }, { "Globals", "isPathWritable:" }, { "Globals", "isPathWritable:canCreateFile:" }, { "NSFileManager", "hasFullDiskAccess" },
	};
	/* views that draw with Core Animation layers (the activity indicator of AppCleaner): their layer code has nothing to work on here, so the setters that only change layers do nothing */
	static const struct { const char *cls, *sel; } nop[] = { { "DJActivityIndicator", "setColor:" } };
	void *(*gim)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod"), *(*gcm)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getClassMethod");
	IMP (*smi)(void *, IMP) = dlsym(RTLD_DEFAULT, "method_setImplementation");
	size_t i;
	for (i = 0; i < sizeof(nop) / sizeof(nop[0]); i++) {
		Class k = rt_getClass(nop[i].cls); void *m;
		if (k && gim && smi && (m = gim(k, rt_registerName(nop[i].sel))) != NULL) { smi(m, (IMP)nop_any); VLOG("objc: patched -[%s %s] (does nothing)\n", nop[i].cls, nop[i].sel); }
	}
	for (i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		Class k = rt_getClass(p[i].cls); void *m;
		if (k == NULL || gim == NULL || smi == NULL) continue;
		if ((m = gim(k, rt_registerName(p[i].sel))) != NULL) { smi(m, (IMP)yes_any); VLOG("objc: patched -[%s %s]\n", p[i].cls, p[i].sel); }
		if (gcm && (m = gcm(k, rt_registerName(p[i].sel))) != NULL) { smi(m, (IMP)yes_any); VLOG("objc: patched +[%s %s]\n", p[i].cls, p[i].sel); }
	}
}

/* ---- audit: the selectors of the program that no class of the runtime implements (the Apple API that GNUstep lacks, or messages to classes that are not there) ---- */
static void
audit_selectors(void *(*find)(const char *, size_t *))
{
	Class *(*copylist)(unsigned *) = dlsym(RTLD_DEFAULT, "objc_copyClassList");
	void *(*gim)(Class, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
	const char *(*cname)(Class) = dlsym(RTLD_DEFAULT, "class_getName");
	size_t n, i; unsigned nc, c, missing = 0; Class *all; uintptr_t *refs = find("__objc_selrefs", &n);
	if (!copylist || !gim || !refs) return;
	all = copylist(&nc);
	for (i = 0; i < n / sizeof(void *); i++) {
		SEL sel = (SEL)refs[i]; int found = 0;
		for (c = 0; c < nc && !found; c++)
			if (gim(all[c], sel) || gim(rt_object_getClass((id)all[c]), sel)) found = 1;
		if (!found) { fprintf(stderr, "audit: no class implements %s\n", rt_selName(sel)); missing++; }
	}
	fprintf(stderr, "audit: %u of %zu selectors are implemented by no class (%u classes)\n", missing, n / sizeof(void *), nc);
	(void)cname; free(all);
}

/* ---- MACHRUN_TRACEMSG=1: every message that the program's own code sends, to stderr ---- */
void *machrun_arg2;
static void *(*traced_lookup)(id, SEL);
static void *
trace_lookup(id obj, SEL sel)
{
	static long count; static pthread_t mainthr; static int init, bg;
	if (!init) { mainthr = pthread_self(); init = 1; bg = getenv("MACHRUN_TRACEBG") != NULL; }
	if (obj != NULL && trace_on && count < 20000 && (!bg || !pthread_equal(pthread_self(), mainthr))) {
		const char *(*cn)(Class) = dlsym(RTLD_DEFAULT, "class_getName");
		Class k = rt_object_getClass(obj);
		const char *n = cn && k ? cn(k) : "?";
		const char *sn = rt_selName(sel);
		{ static const char *g; static int gi; if (!gi) { g = getenv("MACHRUN_TRACEGREP"); gi = 1; } if (g && !strstr(sn, g) && !strstr(n ? n : "", g)) return (traced_lookup(obj, sel)); }
		count++;
		if (strncmp(sn, "performSelector:", 16) == 0 || strncmp(sn, "performSelectorOnMainThread", 27) == 0) fprintf(stderr, "msg %s%s %s  [%s]\n", (void *)k == obj ? "+" : "-", n ? n : "(null)", sn, machrun_arg2 ? rt_selName((SEL)machrun_arg2) : "?");
		else fprintf(stderr, "msg %s%s %s\n", (void *)k == obj ? "+" : "-", n ? n : "(null)", sn);
	}
	return (traced_lookup(obj, sel));
}

/* ---- the entry point ---- */
typedef void *(*find_fn)(const char *sect, size_t *size);

int
objc_load_image(find_fn find, intptr_t slide)
{
	return (objc_load_image2(find, slide, 1));
}

int
objc_load_image2(find_fn find, intptr_t slide, int is_main)
{
	static int objc_inited;
	size_t n, i; void **p; struct o_class **cl; struct o_cat **cats; struct o_proto **prs; uintptr_t *refs;
	size_t ncl = 0, ncat = 0, npr = 0;

	{ void *dp = find("__objc_data", &n); if (dp && getenv("MACHRUN_NOFIX") == NULL) { if (machrun_cls_lo == 0 || (uintptr_t)dp < machrun_cls_lo) machrun_cls_lo = (uintptr_t)dp; if ((uintptr_t)dp + n > machrun_cls_hi) machrun_cls_hi = (uintptr_t)dp + n; } }	/* class structures in the program: see machrun_fix_recv */
	cl = find("__objc_classlist", &n); ncl = n / sizeof(void *);
	cats = find("__objc_catlist", &n); ncat = n / sizeof(void *);
	prs = find("__objc_protolist", &n); npr = n / sizeof(void *);
	if (ncl + ncat + npr == 0 && find("__cfstring", &n) == NULL) return (0);
	if (!objc_inited) {
	objc_inited = 1;
	if (getenv("MACHRUN_SYMBOLS")) symfile = fopen(getenv("MACHRUN_SYMBOLS"), "w");
	if (!rt_init()) { warnx("the program has Objective-C metadata but the Objective-C runtime (libobjc2) could not be used"); return (-1); }
	VLOG("objc: %zu classes, %zu categories, %zu protocols\n", ncl, ncat, npr);
	ib_register_classes();
	gnustep_fixes();
	{	const char *cp = getenv("MACHRUN_COMPAT") ? getenv("MACHRUN_COMPAT") : "libmachcompat.so";	/* GNUstep fixes written in Objective-C */
		if (dlopen(cp, RTLD_NOW | RTLD_GLOBAL) == NULL) VLOG("objc: no %s: %s\n", cp, dlerror()); else VLOG("objc: loaded %s\n", cp);
	}
	add_compat_methods();
	if (getenv("MACHRUN_LENIENT")) setup_lenient();
	if (getenv("MACHRUN_TRACE_AFTER_INVOKE")) trace_on = 0;
	if (getenv("MACHRUN_TRACEMSG")) { traced_lookup = machrun_objc_lookup; machrun_objc_lookup = trace_lookup; }

	}
	if (noimgs < 128) { oimgs[noimgs].cl = cl; oimgs[noimgs].ncl = ncl; noimgs++; }
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
	if (is_main && getenv("MACHRUN_AUDIT")) audit_selectors(find);
	if (is_main) program_patches();
	/* constant strings */
	if ((p = find("__cfstring", &n)) != NULL) {
		setup_cfstring_class();
		if (cfs_class) for (i = 0; i < n / 32; i++) ((void **)p)[i * 4] = cfs_class;
		VLOG("objc: %zu constant strings\n", n / 32);
	}
	if (symfile) fflush(symfile);
	{ struct o_class **nl = find("__objc_nlclslist", &n); if (noimgs > 0) { oimgs[noimgs - 1].nl = nl; oimgs[noimgs - 1].nnl = n / sizeof(void *); } }
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

/* A class that the system does not have (Sparkle, CAEmitterLayer, UTType ...) is an empty placeholder; any message to it, or to its instances, is a no-op that returns 0 */
static long placeholder_nop(id self, SEL _cmd) { return (0); }
static BOOL placeholder_resolve_instance(Class self, SEL _cmd, SEL sel) { rt_addMethod(self, sel, (IMP)placeholder_nop, "@16@0:8"); return (1); }
static BOOL placeholder_resolve_class(Class self, SEL _cmd, SEL sel) { rt_addMethod(rt_object_getClass((id)self), sel, (IMP)placeholder_nop, "@16@0:8"); return (1); }

void
objc_send_retain_release(void *obj, int release)
{
	SEL s;
	if (!rt_init()) return;
	s = rt_registerName(release ? "release" : "retain");
	((void (*)(id, SEL))machrun_objc_lookup((id)obj, s))((id)obj, s);
}

/* libdispatch objects (queues, groups, semaphores) are not Objective-C objects here, but the compiler of a Mac application retains and releases them with objc_retain & co. (on a Mac they are);
 * those calls go to the retain and release of libdispatch for them: an object whose first word points into libdispatch itself is one of its objects. */
#include <link.h>
static uintptr_t dsp_lo, dsp_hi; static int dsp_done;
static int
dsp_cb(struct dl_phdr_info *i, size_t sz, void *arg)
{
	int k;
	if (i->dlpi_name == NULL || strstr(i->dlpi_name, "libdispatch") == NULL) return (0);
	for (k = 0; k < i->dlpi_phnum; k++) if (i->dlpi_phdr[k].p_type == PT_LOAD) {
		uintptr_t a = i->dlpi_addr + i->dlpi_phdr[k].p_vaddr, b = a + i->dlpi_phdr[k].p_memsz;
		if (dsp_lo == 0 || a < dsp_lo) dsp_lo = a;
		if (b > dsp_hi) dsp_hi = b;
	}
	return (1);
}
static inline int
is_dsp(void *o)
{
	uintptr_t w;
	if (o == NULL || ((uintptr_t)o & 7) != 0) return (0);
	if (!dsp_done) { if (getenv("MACHRUN_NODSP") == NULL) dl_iterate_phdr(dsp_cb, NULL); dsp_done = 1; }
	if (dsp_hi == 0) return (0);
	w = *(uintptr_t *)o;
	return (w >= dsp_lo && w < dsp_hi);
}
static void *real(const char *n) { return (dlsym(RTLD_NEXT, n) ? dlsym(RTLD_NEXT, n) : dlsym(RTLD_DEFAULT, n)); }
#define	REAL1(var, name)	static void *(*var)(void *); if (!var) var = real(name)
static void *shim_retain(void *o) { REAL1(f, "objc_retain"); if (is_dsp(o)) { void (*d)(void *) = dlsym(RTLD_DEFAULT, "dispatch_retain"); if (d) d(o); return (o); } return (f(o)); }
static void shim_release(void *o) { static void (*f)(void *); if (!f) f = real("objc_release"); if (is_dsp(o)) { void (*d)(void *) = dlsym(RTLD_DEFAULT, "dispatch_release"); if (d) d(o); return; } f(o); }
static void *shim_retainAutoreleasedReturnValue(void *o) { return (shim_retain(o)); }
static void *shim_autorelease(void *o) { REAL1(f, "objc_autorelease"); return (is_dsp(o) ? o : f(o)); }
static void *shim_autoreleaseReturnValue(void *o) { REAL1(f, "objc_autoreleaseReturnValue"); return (is_dsp(o) ? o : f(o)); }
static void *shim_retainAutorelease(void *o) { REAL1(f, "objc_retainAutorelease"); if (is_dsp(o)) return (shim_retain(o)); return (f(o)); }
static void *shim_retainAutoreleaseReturnValue(void *o) { REAL1(f, "objc_retainAutoreleaseReturnValue"); if (is_dsp(o)) return (shim_retain(o)); return (f(o)); }
static void shim_storeStrong(void **loc, void *val) { void *old = *loc; *loc = shim_retain(val); shim_release(old); }

void *machrun_fix_recv(void *p);
/* the optimized messages of newer compilers (objc_opt_*): plain messages that go through objc_msgSend in Apple's runtime */
static id
send0(id o, const char *sel)
{
	SEL s = rt_registerName(sel);
	return (((id (*)(id, SEL))machrun_objc_lookup(o, s))(o, s));
}
static inline id fixr(id o) { return ((uintptr_t)o >= machrun_cls_lo && (uintptr_t)o < machrun_cls_hi ? (id)machrun_fix_recv(o) : o); }
static id shim_opt_class(id o) { o = fixr(o); return (o ? send0(o, "class") : NULL); }
static id shim_opt_new(id c) { c = fixr(c); return (c ? send0(c, "new") : NULL); }
/* alloc and friends of newer compilers (objc_alloc(Class) ...): the class may be the structure of the program */
static id shim_alloc(id c) { c = fixr(c); return (c ? send0(c, "alloc") : NULL); }
static id
shim_alloc_init(id c)
{
	c = fixr(c);
	if (c == NULL) return (NULL);
	{ id o = send0(c, "alloc"); return (o ? send0(o, "init") : NULL); }
}
static id
shim_allocWithZone(id c, void *zone)
{
	SEL s = rt_registerName("allocWithZone:");
	c = fixr(c);
	return (c ? ((id (*)(id, SEL, void *))machrun_objc_lookup(c, s))(c, s, zone) : NULL);
}
static BOOL
shim_opt_isKindOfClass(id o, Class c)
{
	o = fixr(o); c = (Class)fixr((id)c);
	SEL s = rt_registerName("isKindOfClass:");
	return (o ? ((BOOL (*)(id, SEL, Class))machrun_objc_lookup(o, s))(o, s, c) : 0);
}
static BOOL
shim_opt_respondsToSelector(id o, SEL sel)
{
	o = fixr(o);
	SEL s = rt_registerName("respondsToSelector:");
	return (o ? ((BOOL (*)(id, SEL, SEL))machrun_objc_lookup(o, s))(o, s, sel) : 0);
}
static id shim_identity(id o) { return (o); }

void *
objc_shim_lookup(const char *name)
{
	if (getenv("MACHRUN_NORR") == NULL) {
	if (strcmp(name, "_objc_retain") == 0) return ((void *)shim_retain);
	if (strcmp(name, "_objc_release") == 0) return ((void *)shim_release);
	if (strcmp(name, "_objc_retainAutoreleasedReturnValue") == 0) return ((void *)shim_retainAutoreleasedReturnValue);
	if (strcmp(name, "_objc_autorelease") == 0) return ((void *)shim_autorelease);
	if (strcmp(name, "_objc_autoreleaseReturnValue") == 0) return ((void *)shim_autoreleaseReturnValue);
	if (strcmp(name, "_objc_retainAutorelease") == 0) return ((void *)shim_retainAutorelease);
	if (strcmp(name, "_objc_retainAutoreleaseReturnValue") == 0) return ((void *)shim_retainAutoreleaseReturnValue);
	if (strcmp(name, "_objc_storeStrong") == 0) return ((void *)shim_storeStrong);
	}
	if (getenv("MACHRUN_NOALLOC") == NULL) {
	if (strcmp(name, "_objc_alloc") == 0) return ((void *)shim_alloc);
	if (strcmp(name, "_objc_alloc_init") == 0) return ((void *)shim_alloc_init);
	if (strcmp(name, "_objc_allocWithZone") == 0) return ((void *)shim_allocWithZone);
	if (strcmp(name, "_objc_opt_class") == 0) return ((void *)shim_opt_class);
	if (strcmp(name, "_objc_opt_new") == 0) return ((void *)shim_opt_new);
	if (strcmp(name, "_objc_opt_isKindOfClass") == 0) return ((void *)shim_opt_isKindOfClass);
	if (strcmp(name, "_objc_opt_respondsToSelector") == 0) return ((void *)shim_opt_respondsToSelector);
	if (strcmp(name, "_objc_unsafeClaimAutoreleasedReturnValue") == 0) return ((void *)shim_identity);
	}
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
		if (k == NULL && (strncmp(name + 14, "NSConstant", 10) == 0 || strcmp(name + 14, "UTType") == 0)) {		/* the constant literals of the compiler: classes of libmachcompat.so */
			static int tried;
			if (!tried) { const char *cp = getenv("MACHRUN_COMPAT") ? getenv("MACHRUN_COMPAT") : "libmachcompat.so"; tried = 1; dlopen(cp, RTLD_NOW | RTLD_GLOBAL); }
			k = rt_getClass(name + 14);
		}
		if (k == NULL) {					/* a class that the system does not have: an empty placeholder, so that the program starts */
			Class ns = rt_getClass("NSObject");
			k = rt_allocateClassPair(ns, name + 14, 0);
			if (k) {
				Class meta = rt_object_getClass((id)k);
				rt_addMethod(meta, rt_registerName("resolveInstanceMethod:"), (IMP)placeholder_resolve_instance, "c24@0::16");
				rt_addMethod(meta, rt_registerName("resolveClassMethod:"), (IMP)placeholder_resolve_class, "c24@0::16");
				rt_registerClassPair(k); VLOG("placeholder class %s\n", name + 14);
			}
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

/* A program built by a recent compiler sends messages to its own classes with the address of the class structure ("lea rdi,[rip+_OBJC_CLASS_$_Foo]"; on a Mac the runtime makes that structure the class in
 * place).  Here the class is made by the GNU runtime, so a receiver that is one of those structures is replaced by the class it stands for (the trampolines call this when the receiver is in __objc_data). */
void *
machrun_fix_recv(void *p)
{
	Class k = own_class(p);
	return (k ? (void *)k : p);
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
"testq %rdi,%rdi\n\tjz 1f\n\tmovq %rdx,machrun_arg2(%rip)\n\t" SAVE_REGS "cmpq machrun_cls_lo(%rip),%rdi\n\tjb 3f\n\tcmpq machrun_cls_hi(%rip),%rdi\n\tjae 3f\n\tcall machrun_fix_recv\n\tmovq %rax,0x00(%rsp)\n\tmovq %rax,%rdi\n\tmovq 0x08(%rsp),%rsi\n3:\tcall *machrun_objc_lookup(%rip)\n\tmovq %rax,%r11\n\t" RESTORE_REGS "leave\n\tjmp *%r11\n"
"1:\txorl %eax,%eax\n\txorl %edx,%edx\n\txorps %xmm0,%xmm0\n\txorps %xmm1,%xmm1\n\tret\n"

".globl machrun_objc_msgSend_stret\n.type machrun_objc_msgSend_stret,@function\nmachrun_objc_msgSend_stret:\n\t"
"testq %rsi,%rsi\n\tjz 2f\n\t" SAVE_REGS "movq %rsi,%rdi\n\tmovq %rdx,%rsi\n\tcmpq machrun_cls_lo(%rip),%rdi\n\tjb 4f\n\tcmpq machrun_cls_hi(%rip),%rdi\n\tjae 4f\n\tcall machrun_fix_recv\n\tmovq %rax,0x08(%rsp)\n\tmovq %rax,%rdi\n\tmovq 0x10(%rsp),%rsi\n4:\tcall *machrun_objc_lookup(%rip)\n\tmovq %rax,%r11\n\t" RESTORE_REGS "leave\n\tjmp *%r11\n"
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
/* kCFBundleExecutableKey and the other keys of the Info.plist of CoreFoundation: variables that hold the NSString "CFBundleExecutable" (GNUstep corebase has them as CFString structures that its
 * own runtime has to initialize, which are no objects for the GNUstep Foundation) */
void *
objc_cf_key_cell(const char *name)
{
	static struct { char *name; void **cell; } seen[32]; static int nseen; int i; Class cls; SEL sel; id str; void **cell; char buf[96]; size_t n = strlen(name);
	if (strncmp(name, "_kCFBundle", 10) != 0 || n < 14 || strcmp(name + n - 3, "Key") != 0 || n > 90) return (NULL);
	if (!rt_init() || (cls = rt_getClass("NSString")) == NULL) return (NULL);
	for (i = 0; i < nseen; i++) if (strcmp(seen[i].name, name) == 0) return (seen[i].cell);
	if (nseen == 32) return (NULL);
	memcpy(buf, name + 2, n - 5); buf[n - 5] = 0;				/* "_kCFBundleExecutableKey" -> "CFBundleExecutable" */
	sel = rt_registerName("stringWithUTF8String:");
	str = ((id (*)(id, SEL, const char *))machrun_objc_lookup(cls, sel))(cls, sel, buf);
	{ SEL r = rt_registerName("retain"); ((id (*)(id, SEL))machrun_objc_lookup(str, r))(str, r); }
	cell = malloc(sizeof(void *)); *cell = str;
	seen[nseen].name = strdup(name); seen[nseen++].cell = cell;
	return (cell);
}

void *
objc_constant_fallback(const char *name)
{
	static struct { char *name; void **cell; } seen[256]; static int nseen; int i; Class cls; SEL sel; id str; void **cell;
	/* objects that the compiler refers to directly: the empty array and dictionary, @YES and @NO (the address of the symbol is the object) */
	if (strcmp(name, "___NSArray0__struct") == 0 || strcmp(name, "___NSDictionary0__struct") == 0 || strcmp(name, "___kCFBooleanTrue") == 0 || strcmp(name, "___kCFBooleanFalse") == 0) {
		static id objs[4]; int k = name[3] == 'N' ? (name[5] == 'A' ? 0 : 1) : (name[strlen(name) - 1] == 'e' && name[strlen(name) - 2] == 'u' ? 2 : 3);
		if (objs[k] == NULL && rt_init()) {
			Class c; SEL s; id (*m)(id, SEL);
			if (k < 2) { c = rt_getClass(k ? "NSDictionary" : "NSArray"); s = rt_registerName(k ? "dictionary" : "array"); objs[k] = ((id (*)(id, SEL))machrun_objc_lookup((id)c, s))((id)c, s); }
			else { c = rt_getClass("NSNumber"); s = rt_registerName("numberWithBool:"); objs[k] = ((id (*)(id, SEL, BOOL))machrun_objc_lookup((id)c, s))((id)c, s, k == 2); }
			(void)m;
			if (objs[k]) { SEL r = rt_registerName("retain"); ((id (*)(id, SEL))machrun_objc_lookup(objs[k], r))(objs[k], r); }
		}
		return (objs[k]);
	}
	if (strcmp(name, "__os_log_default") == 0) { static id lo; if (lo == NULL && rt_init()) { id (*ci)(Class, size_t) = dlsym(RTLD_DEFAULT, "class_createInstance"); if (ci) lo = ci(rt_getClass("NSObject"), 0); } return (lo ? (void *)lo : (void *)zero16); }
	if (strncmp(name, "_UTType", 7) == 0 && name[7] >= 'A' && name[7] <= 'Z') {	/* UTType constants: a variable that holds the UTType of libmachcompat.so */
		static void *cells[64]; static char *names[64]; static int n; int q; id (*mk)(const char *);
		for (q = 0; q < n; q++) if (strcmp(names[q], name) == 0) return (&cells[q]);
		{ static int tried; if (!tried) { const char *cp = getenv("MACHRUN_COMPAT") ? getenv("MACHRUN_COMPAT") : "libmachcompat.so"; tried = 1; dlopen(cp, RTLD_NOW | RTLD_GLOBAL); } }
		mk = dlsym(RTLD_DEFAULT, "MachoUTConstant");
		if (n == 64) return (NULL);
		cells[n] = mk ? (void *)mk(name + 7) : NULL; names[n] = strdup(name);
		if (cells[n]) { SEL r = rt_registerName("retain"); ((id (*)(id, SEL))machrun_objc_lookup((id)cells[n], r))((id)cells[n], r); }
		return (&cells[n++]);
	}
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
