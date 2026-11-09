/*
 * machrun: a dyld for FreeBSD.  It maps a Mach-O executable (thin or fat), applies the rebase and bind information
 * (the classic LC_DYLD_INFO_ONLY opcode streams), binds every import to the FreeBSD library that has the same function
 * (shims.c translates what differs: structures, flags, symbol decorations), runs the initializers and calls main().
 * The program runs as an ordinary FreeBSD process: no kernel support is needed, the Mach-O code calls FreeBSD's libc
 * directly (the x86-64 calling convention of Darwin is the System V one).
 *
 *   machrun [-v] PROGRAM [arguments]       -v (or MACHRUN_DEBUG=1) says what is bound and what is missing
 *
 * Not done yet: chained fixups (LC_DYLD_CHAINED_FIXUPS), thread-local variables, Objective-C metadata, dependent dylibs
 * that are Mach-O files, arm64 (the variadic calling convention of Darwin's arm64 differs from the standard one).
 */
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/endian.h>
#include <sys/sysctl.h>

#include "macho.h"
#include "machrun.h"

#define	LC_SYMTAB		0x2
#define	LC_DYLD_INFO_ONLY	(0x22 | LC_REQ_DYLD)
#define	LC_DYLD_INFO		0x22
#define	LC_LOAD_WEAK_DYLIB	(0x18 | LC_REQ_DYLD)
#define	LC_REEXPORT_DYLIB	(0x1f | LC_REQ_DYLD)
#define	LC_LOAD_UPWARD_DYLIB	(0x23 | LC_REQ_DYLD)
#define	LC_LAZY_LOAD_DYLIB	0x20
#define	LC_DYLD_CHAINED_FIXUPS	(0x34 | LC_REQ_DYLD)
#define	LC_DYLD_EXPORTS_TRIE	(0x33 | LC_REQ_DYLD)
#define	LC_RPATH		(0x1c | LC_REQ_DYLD)
#define	LC_ID_DYLIB		0xd
#define	MH_DYLIB_TYPE		0x6

#define	S_MOD_INIT_FUNC_POINTERS 0x9
#define	S_INIT_FUNC_OFFSETS	0x16
#define	S_THREAD_LOCAL_VARIABLES 0x13

struct dyld_info_command {
	uint32_t cmd, cmdsize;
	uint32_t rebase_off, rebase_size, bind_off, bind_size, weak_bind_off, weak_bind_size,
	    lazy_bind_off, lazy_bind_size, export_off, export_size;
};
struct section_64 {
	char sectname[16], segname[16];
	uint64_t addr, size;
	uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};


#define	MAXSEG	16
#define	MAXLIB	64
struct seg {
	char		name[17];
	uint64_t	vmaddr, vmsize, fileoff, filesize;
	uint32_t	initprot, nsects;
	const struct section_64 *sects;
};

struct image {
	const uint8_t	*file;			/* the mapped file (the slice) */
	size_t		size;
	struct seg	seg[MAXSEG];
	int		nseg;
	char		*libname[MAXLIB];	/* ordinal - 1 */
	void		*libhandle[MAXLIB];
	int		nlib;
	const struct dyld_info_command *dyld_info;
	bool		chained;
	const uint8_t	*chained_data;		/* the LC_DYLD_CHAINED_FIXUPS blob */
	uint64_t	min_vm, max_vm;		/* preferred addresses of the mapped part */
	uint64_t	text_vm;		/* preferred address of __TEXT: LC_MAIN's offset is from here */
	intptr_t	slide;
	uint64_t	entryoff;
	bool		have_main;
	uint64_t	thread_pc;
	bool		have_thread;
	uint32_t	flags;
	/* dylibs that are Mach-O files (libmpv, FFmpeg ...) */
	char		*path;			/* real path of the file */
	char		*idname;		/* LC_ID_DYLIB */
	struct image	*libimg[MAXLIB];	/* ordinal - 1: the Mach-O image that provides that library, or NULL (a library of this system) */
	bool		libre[MAXLIB];		/* LC_REEXPORT_DYLIB */
	const uint8_t	*exports; uint32_t export_size;	/* export trie */
	char		*rpath[8]; int nrpath;
	bool		is_dylib, bound, inited, objc_done;
	struct image	*next;
};

int machrun_verbose;
static const char *launcher_real;			/* set in launcher mode: the Mach-O file to load */
static unsigned unresolved_count;

/* ---- stubs for symbols that nothing implements: the call says which one and stops ---- */

static uint8_t *stubarea; static size_t stubused, stubsize = 1 << 17;

void
machrun_unresolved(const char *const *name)
{
	fprintf(stderr, "machrun: the program called the unimplemented symbol %s\n", *name);
	{	/* who called it: the frames that are in a Mach-O image (rbp chain) */
		void **fp = __builtin_frame_address(0); int n = 0;
		while (fp && n < 24 && ((uintptr_t)fp & 7) == 0 && (uintptr_t)fp > 0x10000) {
			uintptr_t ra = (uintptr_t)fp[1]; struct image *d;
			extern struct image *machrun_images_head(void);
			for (d = machrun_images_head(); d != NULL; d = d->next)
				if (ra >= d->min_vm + d->slide && ra < d->max_vm + d->slide) { fprintf(stderr, "  called from %s + %#lx (address %#lx in the file)\n", d->path, (unsigned long)(ra - d->slide - d->min_vm), (unsigned long)(ra - d->slide)); break; }
			if ((void **)fp[0] <= fp) break;
			fp = (void **)fp[0]; n++;
		}
	}
	_exit(127);
}

static void *
make_stub(const char *name)
{
	uint8_t *s;

	if (stubarea == NULL) {
		stubarea = mmap(NULL, stubsize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		if (stubarea == MAP_FAILED)
			err(1, "mmap stubs");
	}
	if (stubused + 32 > stubsize)
		errx(1, "too many unresolved symbols");
	mprotect(stubarea, stubsize, PROT_READ | PROT_WRITE);	/* it is read-only and executable after the first image was protected */
	s = stubarea + stubused; stubused += 32;
	*(const char **)(s + 16) = strdup(name);
	*(void **)(s + 24) = (void *)machrun_unresolved;
#if defined(__x86_64__)
	/* lea rdi,[rip+9] ; jmp [rip+11] */
	static const uint8_t code[13] = { 0x48, 0x8d, 0x3d, 9, 0, 0, 0, 0xff, 0x25, 11, 0, 0, 0 };
	memcpy(s, code, sizeof(code));
#else
	static const uint32_t code[4] = { 0x10000080 /* adr x0,#16 */, 0x580000A1 /* ldr x1,#20 */, 0xD61F0020 /* br x1 */, 0xD503201F };
	memcpy(s, code, sizeof(code));
#endif
	mprotect(stubarea, stubsize, PROT_READ | PROT_EXEC);
	return (s);
}

/* ---- the file ---- */

static uint64_t
uleb(const uint8_t **p, const uint8_t *end)
{
	uint64_t r = 0; int shift = 0;
	while (*p < end) {
		uint8_t b = *(*p)++;
		r |= (uint64_t)(b & 0x7f) << shift;
		shift += 7;
		if (!(b & 0x80)) break;
	}
	return (r);
}
static int64_t
sleb(const uint8_t **p, const uint8_t *end)
{
	int64_t r = 0; int shift = 0; uint8_t b = 0;
	while (*p < end) {
		b = *(*p)++;
		r |= (int64_t)(b & 0x7f) << shift;
		shift += 7;
		if (!(b & 0x80)) break;
	}
	if (shift < 64 && (b & 0x40)) r |= -((int64_t)1 << shift);
	return (r);
}

static void
parse(struct image *im, const char *path)
{
	struct stat st; int fd = open(path, O_RDONLY);
	const uint8_t *m; const struct mach_header_64 *mh; const uint8_t *cmd; uint32_t i;
	const uint32_t hostcpu =
#if defined(__x86_64__)
	    CPU_TYPE_X86_64;
#else
	    CPU_TYPE_ARM64;
#endif

	if (fd < 0 || fstat(fd, &st) < 0)
		err(1, "%s", path);
	m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED)
		err(1, "mmap %s", path);
	close(fd);
	im->file = m; im->size = st.st_size;

	if (be32dec(m) == FAT_MAGIC) {
		uint32_t n = be32dec(m + 4); int found = 0;
		for (i = 0; i < n && i < 16; i++) {
			const uint8_t *fa = m + 8 + 20 * i;
			if (be32dec(fa) == hostcpu && (!found || (be32dec(fa + 4) & 0xff) == 0)) {
				im->file = m + be32dec(fa + 8); im->size = be32dec(fa + 12); found = 1;
			}
		}
		if (!found)
			errx(1, "%s: no slice for this architecture", path);
	}
	mh = (const struct mach_header_64 *)im->file;
	if (mh->magic != MH_MAGIC_64 || mh->cputype != hostcpu || (mh->filetype != MH_EXECUTE && mh->filetype != MH_DYLIB_TYPE && mh->filetype != 8 /* MH_BUNDLE */))
		errx(1, "%s: not a Mach-O executable or dynamic library for this architecture", path);
	im->is_dylib = mh->filetype != MH_EXECUTE;
	im->flags = mh->flags;
	im->min_vm = ~0ULL;

	cmd = (const uint8_t *)(mh + 1);
	for (i = 0; i < mh->ncmds; i++, cmd += ((const struct load_command *)cmd)->cmdsize) {
		const struct load_command *lc = (const struct load_command *)cmd;
		switch (lc->cmd) {
		case LC_SEGMENT_64: {
			const struct segment_command_64 *sc = (const void *)lc; struct seg *s;
			if (im->nseg == MAXSEG) errx(1, "too many segments");
			s = &im->seg[im->nseg++];
			memcpy(s->name, sc->segname, 16); s->name[16] = 0;
			s->vmaddr = sc->vmaddr; s->vmsize = sc->vmsize; s->fileoff = sc->fileoff; s->filesize = sc->filesize;
			s->initprot = sc->initprot; s->nsects = sc->nsects; s->sects = (const struct section_64 *)(sc + 1);
			if (strcmp(s->name, "__TEXT") == 0) im->text_vm = sc->vmaddr;
			if (strcmp(s->name, "__PAGEZERO") != 0 && sc->vmsize) {
				if (sc->vmaddr < im->min_vm) im->min_vm = sc->vmaddr;
				if (sc->vmaddr + sc->vmsize > im->max_vm) im->max_vm = sc->vmaddr + sc->vmsize;
			}
			break;
		}
		case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
			im->dyld_info = (const void *)lc;
			if (((const struct dyld_info_command *)lc)->export_size) { im->exports = im->file + ((const struct dyld_info_command *)lc)->export_off; im->export_size = ((const struct dyld_info_command *)lc)->export_size; }
			break;
		case LC_DYLD_EXPORTS_TRIE:
			im->exports = im->file + ((const uint32_t *)lc)[2]; im->export_size = ((const uint32_t *)lc)[3]; break;
		case LC_RPATH:
			if (im->nrpath < 8) im->rpath[im->nrpath++] = strndup((const char *)lc + ((const uint32_t *)lc)[2], lc->cmdsize - ((const uint32_t *)lc)[2]);
			break;
		case LC_ID_DYLIB:
			im->idname = strndup((const char *)lc + ((const uint32_t *)lc)[2], lc->cmdsize - ((const uint32_t *)lc)[2]); break;
		case LC_DYLD_CHAINED_FIXUPS:
			im->chained = true; im->chained_data = im->file + ((const uint32_t *)lc)[2]; break;
		case LC_MAIN:
			im->have_main = true; im->entryoff = ((const struct entry_point_command *)lc)->entryoff; break;
		case LC_UNIXTHREAD: {
			const uint32_t *state = (const uint32_t *)((const struct thread_command *)lc + 1);
#if defined(__x86_64__)
			memcpy(&im->thread_pc, state + 2 * X86_STATE64_RIP_INDEX, 8);
#else
			memcpy(&im->thread_pc, state + 2 * ARM_STATE64_PC_INDEX, 8);
#endif
			im->have_thread = true; break;
		}
		case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB: case LC_LAZY_LOAD_DYLIB: {
			uint32_t off = ((const uint32_t *)lc)[2];
			if (im->nlib == MAXLIB) errx(1, "too many libraries");
			im->libre[im->nlib] = lc->cmd == LC_REEXPORT_DYLIB;
			im->libname[im->nlib++] = strndup((const char *)lc + off, lc->cmdsize - off);
			break;
		}
		}
	}
	if (im->nseg == 0 || im->min_vm == ~0ULL)
		errx(1, "%s: no segments", path);
}

static void
map_image(struct image *im)
{
	uint64_t span = round_page(im->max_vm - im->min_vm);
	void *a = im->min_vm >= 0x100000 ? mmap((void *)im->min_vm, span, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_EXCL, -1, 0) : MAP_FAILED;
	int i;

	if (a == MAP_FAILED)	/* the preferred address is taken: slide (a multiple of 16 KB, the Mach-O page size) */
		a = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_ALIGNED(14), -1, 0);
	if (a == MAP_FAILED)
		err(1, "mmap image");
	im->slide = (intptr_t)a - (intptr_t)im->min_vm;
	VLOG("image at %p (slide %#lx, %lu bytes)\n", a, (long)im->slide, (unsigned long)span);
	for (i = 0; i < im->nseg; i++) {
		struct seg *s = &im->seg[i];
		if (strcmp(s->name, "__PAGEZERO") == 0 || s->vmsize == 0)
			continue;
		if (s->fileoff + s->filesize > im->size)
			errx(1, "segment %s is outside of the file", s->name);
		memcpy((void *)(s->vmaddr + im->slide), im->file + s->fileoff, s->filesize);
	}
}

/* ---- libraries and symbols ---- */

extern char **environ;
struct image *machrun_images_head(void);
static struct image *images;				/* every Mach-O file that is loaded: the program and its dylibs */
static struct image *main_image;
static bool lib_mode;

struct image *machrun_images_head(void) { return (images ? images : main_image); }

/* The export trie of an image: the address of a symbol (a re-exported one is looked up in the library that has it). */
static uintptr_t image_export(struct image *im, const char *name, int depth);

/* The edges of a node are tried one after the other (the trie of some linkers is not compact: "error" and "error_str" can be two edges of the same node) */
static uintptr_t
trie_walk(struct image *im, const uint8_t *node, const char *name, const char *full, int depth, int level)
{
	const uint8_t *base = im->exports, *end = im->exports + im->export_size;
	const uint8_t *p = node; uint64_t tsize; const uint8_t *term, *ch; uint8_t n;
	if (level > 512 || node >= end) return (0);
	tsize = uleb(&p, end); term = p; ch = p + tsize;
	if (*name == 0 && tsize != 0) {
		uint64_t flags = uleb(&term, end);
		if (flags & 0x8) {				/* EXPORT_SYMBOL_FLAGS_REEXPORT: (ordinal, name in that library) */
			uint64_t ord = uleb(&term, end); const char *in = (const char *)term;
			if (ord >= 1 && ord <= (uint64_t)im->nlib && im->libimg[ord - 1] != NULL && depth < 8)
				return (image_export(im->libimg[ord - 1], *in ? in : full, depth + 1));
			return (0);
		}
		{ uint64_t off = uleb(&term, end);
		  if (flags & 0x10) {				/* stub and resolver: the resolver gives the address */
			uint64_t res = uleb(&term, end);
			return (((uintptr_t (*)(void))(im->min_vm + im->slide + res))());
		  }
		  return (im->min_vm + im->slide + off); }
	}
	n = *ch++;
	while (n--) {
		const char *edge = (const char *)ch; size_t el = strlen(edge); uint64_t off;
		ch += el + 1; off = uleb(&ch, end);
		if (strncmp(name, edge, el) == 0) {
			uintptr_t r = trie_walk(im, base + off, name + el, full, depth, level + 1);
			if (r) return (r);
		}
	}
	return (0);
}

static uintptr_t
trie_lookup(struct image *im, const char *name, int depth)
{
	if (im->exports == NULL) return (0);
	return (trie_walk(im, im->exports, name, name, depth, 0));
}

static uintptr_t
image_export(struct image *im, const char *name, int depth)
{
	uintptr_t a = trie_lookup(im, name, depth); int i;
	if (a) return (a);
	for (i = 0; i < im->nlib && depth < 8; i++)		/* the libraries that it re-exports as a whole */
		if (im->libre[i] && im->libimg[i] && (a = image_export(im->libimg[i], name, depth + 1)) != 0) return (a);
	return (0);
}

/* where a dependent library is: @rpath of the image and of the program, @executable_path, @loader_path, or the path itself */
static char *
find_dylib(struct image *im, const char *inst)
{
	char buf[PATH_MAX + 64], dir[PATH_MAX]; const char *rest; int i;
	/* Sparkle (the updater of a Mac application) stays what it was: classes that do nothing (compat.m); MACHRUN_REAL_SPARKLE=1 loads the framework of the application */
	if (strstr(inst, "Sparkle.framework") != NULL && getenv("MACHRUN_REAL_SPARKLE") == NULL) return (NULL);
	snprintf(dir, sizeof(dir), "%s", im->path); { char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else strcpy(dir, "."); }
	if (strncmp(inst, "@rpath/", 7) == 0) {
		struct image *cand[2] = { im, main_image }; int c;
		rest = inst + 7;
		for (c = 0; c < 2; c++) for (i = 0; i < cand[c]->nrpath; i++) {
			const char *rp = cand[c]->rpath[i]; char cdir[PATH_MAX];
			snprintf(cdir, sizeof(cdir), "%s", cand[c]->path); { char *sl = strrchr(cdir, '/'); if (sl) *sl = 0; }
			if (strncmp(rp, "@loader_path", 12) == 0) snprintf(buf, sizeof(buf), "%s%s/%s", cdir, rp + 12, rest);
			else if (strncmp(rp, "@executable_path", 16) == 0) { char mdir[PATH_MAX]; snprintf(mdir, sizeof(mdir), "%s", main_image->path); { char *sl = strrchr(mdir, '/'); if (sl) *sl = 0; } snprintf(buf, sizeof(buf), "%s%s/%s", mdir, rp + 16, rest); }
			else snprintf(buf, sizeof(buf), "%s/%s", rp, rest);
			if (access(buf, R_OK) == 0) return (strdup(buf));
		}
		snprintf(buf, sizeof(buf), "%s/%s", dir, rest); if (access(buf, R_OK) == 0) return (strdup(buf));
		return (NULL);
	}
	if (strncmp(inst, "@loader_path/", 13) == 0) { snprintf(buf, sizeof(buf), "%s/%s", dir, inst + 13); return (access(buf, R_OK) == 0 ? strdup(buf) : NULL); }
	if (strncmp(inst, "@executable_path/", 17) == 0) {
		char mdir[PATH_MAX]; snprintf(mdir, sizeof(mdir), "%s", main_image->path); { char *sl = strrchr(mdir, '/'); if (sl) *sl = 0; }
		snprintf(buf, sizeof(buf), "%s/%s", mdir, inst + 17); return (access(buf, R_OK) == 0 ? strdup(buf) : NULL);
	}
	if (inst[0] == '/' && strncmp(inst, "/usr/lib/", 9) != 0 && strncmp(inst, "/System/", 8) != 0 && access(inst, R_OK) == 0) return (strdup(inst));
	return (NULL);
}

static bool
is_macho_file(const char *path)
{
	uint8_t m[4]; int fd = open(path, O_RDONLY); bool r;
	if (fd < 0) return (false);
	r = read(fd, m, 4) == 4 && (be32dec(m) == FAT_MAGIC || le32dec(m) == MH_MAGIC_64);
	close(fd);
	return (r);
}

static void
load_deps(struct image *im)
{
	int i;
	for (i = 0; i < im->nlib; i++) {
		char *p = find_dylib(im, im->libname[i]), real[PATH_MAX]; struct image *d;
		if (p != NULL && is_macho_file(p) && realpath(p, real) != NULL) {
			for (d = images; d != NULL; d = d->next) if (strcmp(d->path, real) == 0) break;
			if (d == NULL) {
				d = calloc(1, sizeof(*d)); d->path = strdup(real);
				parse(d, real); map_image(d);
				d->next = images; images = d;			/* before its own dependencies: a cycle finds it */
				VLOG("dylib %s at %#lx\n", real, (unsigned long)(d->min_vm + d->slide));
				load_deps(d);
			}
			im->libimg[i] = d;
		} else
			im->libhandle[i] = shim_open_library(im->libname[i]);
		free(p);
		VLOG("library %d: %s%s\n", i + 1, im->libname[i], im->libimg[i] ? " (Mach-O)" : "");
	}
}

static uintptr_t
resolve(struct image *im, int ordinal, const char *name, bool weak)
{
	void *h = NULL, *a = NULL; struct image *d;
	if (ordinal >= 1 && ordinal <= im->nlib) {
		h = im->libhandle[ordinal - 1];
		if (im->libimg[ordinal - 1] != NULL && (a = (void *)image_export(im->libimg[ordinal - 1], name, 0)) != NULL) return ((uintptr_t)a);
	} else if (ordinal <= 0 && ordinal != -3) {			/* the image itself, the program, flat lookup: every Mach-O image */
		if (ordinal == 0 && (a = (void *)image_export(im, name, 0)) != NULL) return ((uintptr_t)a);
		for (d = main_image; d != NULL && a == NULL; d = d == main_image ? images : d->next) if (d != im || ordinal != 0) a = (void *)image_export(d, name, 0);
		if (a != NULL) return ((uintptr_t)a);
	}
	a = shim_lookup(h, name);
	if (strncmp(name, "_CF", 3) == 0) VLOG("bind %s -> %p\n", name, a);
	if (a == NULL) {
		if (weak) return (0);
		unresolved_count++;
		VLOG("UNRESOLVED %s (%s, ordinal %d of %d, image %s)\n", name, im->path ? im->path : "?", ordinal, im->nlib, ordinal >= 1 && ordinal <= im->nlib && im->libimg[ordinal - 1] ? im->libimg[ordinal - 1]->path : "none");
		return ((uintptr_t)make_stub(name));
	}
	return ((uintptr_t)a);
}

/* ---- rebase and bind ---- */

static uint8_t *
seg_addr(struct image *im, int seg, uint64_t off)
{
	if (seg < 0 || seg >= im->nseg) errx(1, "bad segment index %d in the bind information", seg);
	return ((uint8_t *)(im->seg[seg].vmaddr + im->slide + off));
}

/* ---- chained fixups (LC_DYLD_CHAINED_FIXUPS, what the linker makes for macOS 12 and later): every pointer that needs a fix is part of a chain that starts at the page
 * start of its segment; a pointer is either a rebase (its target) or a bind (the number of an import), and says how far the next one is (4 bytes each). ---- */

static void
do_chained(struct image *im)
{
	const uint8_t *h = im->chained_data;
	uint32_t starts_off = le32dec(h + 4), imports_off = le32dec(h + 8), syms_off = le32dec(h + 12), nimports = le32dec(h + 16), ifmt = le32dec(h + 20);
	const uint8_t *starts = h + starts_off; uint32_t nseg = le32dec(starts), s, k;
	uintptr_t *imp = calloc(nimports ? nimports : 1, sizeof(uintptr_t)); int64_t *addend = calloc(nimports ? nimports : 1, sizeof(int64_t));
	uint32_t i;

	for (i = 0; i < nimports; i++) {
		uint32_t ord, nameoff; bool weak; int64_t ad = 0;
		if (ifmt == 1) { uint32_t v = le32dec(h + imports_off + 4 * i); ord = v & 0xff; weak = (v >> 8) & 1; nameoff = v >> 9; }
		else if (ifmt == 2) { uint32_t v = le32dec(h + imports_off + 8 * i); ord = v & 0xff; weak = (v >> 8) & 1; nameoff = v >> 9; ad = (int32_t)le32dec(h + imports_off + 8 * i + 4); }
		else { uint64_t v = le64dec(h + imports_off + 16 * i); ord = v & 0xffff; weak = (v >> 16) & 1; nameoff = v >> 32; ad = (int64_t)le64dec(h + imports_off + 16 * i + 8); if (ord > 0xfff0) ord = 0; }
		if (ifmt != 3 && ord > 0xf0) ord = 0;			/* self / main executable / flat lookup: search everywhere */
		imp[i] = resolve(im, (int)ord, (const char *)h + syms_off + nameoff, weak); addend[i] = ad;
	}
	for (s = 0; s < nseg && s < (uint32_t)im->nseg; s++) {
		uint32_t so = le32dec(starts + 4 + 4 * s); const uint8_t *si; uint16_t psize, fmt, pcount; uint64_t segoff;
		if (so == 0) continue;
		si = starts + so; psize = le16dec(si + 4); fmt = le16dec(si + 6); segoff = le64dec(si + 8); pcount = le16dec(si + 20);
		if (fmt != 2 && fmt != 6) errx(1, "chained fixups: pointer format %u is not supported (only DYLD_CHAINED_PTR_64 and _64_OFFSET)", fmt);
		for (k = 0; k < pcount; k++) {
			uint16_t ps = le16dec(si + 22 + 2 * k); uint8_t *p; 
			if (ps == 0xffff) continue;
			p = (uint8_t *)(im->min_vm + segoff + im->slide + (uint64_t)k * psize + ps);
			for (;;) {
				uint64_t v; uint32_t next;
				memcpy(&v, p, 8); next = (v >> 51) & 0xfff;
				if ((v >> 63) & 1) {					/* bind */
					uint32_t o = v & 0xffffff; int64_t ad = (int64_t)((v >> 24) & 0xff) + (o < nimports ? addend[o] : 0);
					uintptr_t r = o < nimports ? imp[o] : 0;
					v = r ? r + ad : 0;
				} else {						/* rebase */
					uint64_t t = v & 0xfffffffffULL, hi = (v >> 36) & 0xff;
					v = (fmt == 6 ? (uint64_t)im->min_vm + t : t) + im->slide + (hi << 56);
				}
				memcpy(p, &v, 8);
				if (next == 0) break;
				p += (size_t)next * 4;
			}
		}
	}
	free(imp); free(addend);
}

static void
do_rebase(struct image *im)
{
	const struct dyld_info_command *di = im->dyld_info;
	const uint8_t *p = im->file + di->rebase_off, *end = p + di->rebase_size;
	int seg = 0; uint64_t off = 0, count, skip; int type = 1;
#define	REBASE_ONE()	do { if (type == 1) *(uintptr_t *)seg_addr(im, seg, off) += im->slide; } while (0)
	while (p < end) {
		uint8_t op = *p & 0xF0, imm = *p & 0x0F; p++;
		switch (op) {
		case 0x00: return;				/* DONE */
		case 0x10: type = imm; break;			/* SET_TYPE_IMM */
		case 0x20: seg = imm; off = uleb(&p, end); break;
		case 0x30: off += uleb(&p, end); break;
		case 0x40: off += imm * sizeof(void *); break;
		case 0x50: case 0x60:				/* DO_REBASE_IMM_TIMES, DO_REBASE_ULEB_TIMES */
			count = op == 0x50 ? imm : uleb(&p, end);
			for (uint64_t i = 0; i < count; i++) { REBASE_ONE(); off += sizeof(void *); }
			break;
		case 0x70:					/* DO_REBASE_ADD_ADDR_ULEB */
			REBASE_ONE(); off += sizeof(void *) + uleb(&p, end);
			break;
		case 0x80:					/* DO_REBASE_ULEB_TIMES_SKIPPING_ULEB */
			count = uleb(&p, end); skip = uleb(&p, end);
			for (uint64_t i = 0; i < count; i++) { REBASE_ONE(); off += sizeof(void *) + skip; }
			break;
		default: errx(1, "unknown rebase opcode %#x", op);
		}
	}
#undef REBASE_ONE
}

static void
do_bind(struct image *im, uint32_t off, uint32_t size, bool lazy)
{
	const uint8_t *p = im->file + off, *end = p + size;
	int seg = 0, ordinal = 0; uint64_t segoff = 0; int64_t addend = 0; const char *sym = ""; bool weak = false;
	uint64_t count, skip;
	if (size == 0) return;
	while (p < end) {
		uint8_t op = *p & 0xF0, imm = *p & 0x0F; p++;
		switch (op) {
		case 0x00: if (lazy) { addend = 0; break; } return;		/* DONE: ends each entry of the lazy stream */
		case 0x10: ordinal = imm; break;
		case 0x20: ordinal = (int)uleb(&p, end); break;
		case 0x30: ordinal = imm ? (int8_t)(0xF0 | imm) : 0; break;	/* special: 0 self, -1 main, -2 flat, -3 weak */
		case 0x40: weak = imm & 1; sym = (const char *)p; p += strlen(sym) + 1; break;
		case 0x50: break;						/* SET_TYPE_IMM: pointers only */
		case 0x60: addend = sleb(&p, end); break;
		case 0x70: seg = imm; segoff = uleb(&p, end); break;
		case 0x80: segoff += uleb(&p, end); break;
		case 0x90: case 0xA0: case 0xB0: case 0xC0: {
			uintptr_t v = resolve(im, ordinal, sym, weak);
			*(uintptr_t *)seg_addr(im, seg, segoff) = v ? v + addend : 0;
			if (op == 0x90) segoff += sizeof(void *);
			else if (op == 0xA0) segoff += sizeof(void *) + uleb(&p, end);
			else if (op == 0xB0) segoff += sizeof(void *) + imm * sizeof(void *);
			else {
				count = uleb(&p, end); skip = uleb(&p, end);
				for (uint64_t i = 1; i < count; i++) {
					segoff += sizeof(void *) + skip;
					*(uintptr_t *)seg_addr(im, seg, segoff) = v ? v + addend : 0;
				}
				segoff += sizeof(void *) + skip;
			}
			break;
		}
		default: errx(1, "unknown bind opcode %#x", op);
		}
	}
}

static void
protect_image(struct image *im)
{
	int i;
	for (i = 0; i < im->nseg; i++) {
		struct seg *s = &im->seg[i]; int prot = 0;
		if (strcmp(s->name, "__PAGEZERO") == 0 || s->vmsize == 0) continue;
		if (s->initprot & VMP_READ) prot |= PROT_READ;
		if (s->initprot & VMP_WRITE) prot |= PROT_WRITE;
		if (s->initprot & VMP_EXEC) prot |= PROT_EXEC;
		if (mprotect((void *)(s->vmaddr + im->slide), round_page(s->vmsize), prot) < 0)
			warn("mprotect %s", s->name);
	}
	if (stubarea) mprotect(stubarea, stubsize, PROT_READ | PROT_EXEC);
}

static struct image *g_im;
static void *
find_sect(const char *name, size_t *size)
{
	int i; uint32_t j;
	for (i = 0; i < g_im->nseg; i++)
		for (j = 0; j < g_im->seg[i].nsects; j++) {
			const struct section_64 *sc = &g_im->seg[i].sects[j];
			if (strncmp(sc->sectname, name, 16) == 0) { *size = sc->size; return ((void *)(sc->addr + g_im->slide)); }
		}
	*size = 0;
	return (NULL);
}
int objc_load_image(void *(*find)(const char *, size_t *), intptr_t slide);
int objc_load_image2(void *(*find)(const char *, size_t *), intptr_t slide, int is_main);
void objc_call_loads(void);

typedef void (*initfn)(int, char **, char **, char **);

static void
run_initializers(struct image *im, int argc, char **argv, char **envp, char **apple)
{
	int i; uint32_t j;
	for (i = 0; i < im->nseg; i++) {
		for (j = 0; j < im->seg[i].nsects; j++) {
			const struct section_64 *sc = &im->seg[i].sects[j];
			uint32_t type = sc->flags & 0xff; uint64_t k;
			if (type == S_MOD_INIT_FUNC_POINTERS) {
				initfn *f = (initfn *)(sc->addr + im->slide);
				for (k = 0; k < sc->size / sizeof(void *); k++) { VLOG("initializer %p\n", (void *)f[k]); f[k](argc, argv, envp, apple); }
			} else if (type == S_INIT_FUNC_OFFSETS) {
				uint32_t *f = (uint32_t *)(sc->addr + im->slide);
				for (k = 0; k < sc->size / 4; k++) ((initfn)(im->min_vm + im->slide + f[k]))(argc, argv, envp, apple);
			} else if (type == S_THREAD_LOCAL_VARIABLES)
				(void)0;
		}
	}
}

/* ---- thread-local variables (__thread in the dylibs: libjpeg, LuaJIT ...): a descriptor is {thunk, key, offset}; the thunk is called with the descriptor and answers the address of the variable of this thread,
 * keeping every register but rax.  Each image has the initial contents (__thread_data) and the zeroes (__thread_bss) of its variables, copied for a thread the first time it asks. ---- */
#include <pthread.h>
struct tlvinfo { pthread_key_t key; const uint8_t *init; size_t initsize, size; };

void *machrun_tlv_get(uintptr_t *desc);
void *
machrun_tlv_get(uintptr_t *desc)
{
	struct tlvinfo *ti = (struct tlvinfo *)desc[1]; uint8_t *b;
	if (ti == NULL) return (NULL);
	if ((b = pthread_getspecific(ti->key)) == NULL) {
		b = calloc(1, ti->size ? ti->size : 1);
		if (ti->init) memcpy(b, ti->init, ti->initsize);
		pthread_setspecific(ti->key, b);
	}
	return (b + desc[2]);
}
#if defined(__x86_64__)
__asm__(
".text\n.globl machrun_tlv_thunk\n.type machrun_tlv_thunk,@function\nmachrun_tlv_thunk:\n\t"
"pushq %rbp\n\tmovq %rsp,%rbp\n\tpushq %rdi\n\tpushq %rsi\n\tpushq %rdx\n\tpushq %rcx\n\tpushq %r8\n\tpushq %r9\n\tpushq %r10\n\tpushq %r11\n\tsubq $0x80,%rsp\n\t"
"movdqu %xmm0,0x00(%rsp)\n\tmovdqu %xmm1,0x10(%rsp)\n\tmovdqu %xmm2,0x20(%rsp)\n\tmovdqu %xmm3,0x30(%rsp)\n\tmovdqu %xmm4,0x40(%rsp)\n\tmovdqu %xmm5,0x50(%rsp)\n\tmovdqu %xmm6,0x60(%rsp)\n\tmovdqu %xmm7,0x70(%rsp)\n\t"
"call machrun_tlv_get\n\t"
"movdqu 0x00(%rsp),%xmm0\n\tmovdqu 0x10(%rsp),%xmm1\n\tmovdqu 0x20(%rsp),%xmm2\n\tmovdqu 0x30(%rsp),%xmm3\n\tmovdqu 0x40(%rsp),%xmm4\n\tmovdqu 0x50(%rsp),%xmm5\n\tmovdqu 0x60(%rsp),%xmm6\n\tmovdqu 0x70(%rsp),%xmm7\n\t"
"addq $0x80,%rsp\n\tpopq %r11\n\tpopq %r10\n\tpopq %r9\n\tpopq %r8\n\tpopq %rcx\n\tpopq %rdx\n\tpopq %rsi\n\tpopq %rdi\n\tpopq %rbp\n\tret\n"
".globl machrun_chkstk_darwin\n.type machrun_chkstk_darwin,@function\nmachrun_chkstk_darwin:\n\tret\n"
);
#else
void machrun_tlv_thunk(void) {}
void machrun_chkstk_darwin(void) {}
#endif
extern void machrun_tlv_thunk(void), machrun_chkstk_darwin(void);
void *machrun_tlv_thunk_addr(void) { return ((void *)machrun_tlv_thunk); }
void *machrun_chkstk_addr(void) { return ((void *)machrun_chkstk_darwin); }

static void
setup_tlv(struct image *im)
{
	int i; uint32_t j; uint64_t lo = ~0ULL, hi = 0, initend = 0; struct tlvinfo *ti = NULL;
	for (i = 0; i < im->nseg; i++) for (j = 0; j < im->seg[i].nsects; j++) {
		const struct section_64 *sc = &im->seg[i].sects[j]; uint32_t t = sc->flags & 0xff;
		if (t == 0x11 || t == 0x12) { if (sc->addr < lo) lo = sc->addr; if (sc->addr + sc->size > hi) hi = sc->addr + sc->size; if (t == 0x11 && sc->addr + sc->size > initend) initend = sc->addr + sc->size; }
	}
	for (i = 0; i < im->nseg; i++) for (j = 0; j < im->seg[i].nsects; j++) {
		const struct section_64 *sc = &im->seg[i].sects[j]; uint64_t k;
		if ((sc->flags & 0xff) != S_THREAD_LOCAL_VARIABLES) continue;
		if (ti == NULL) {
			ti = calloc(1, sizeof(*ti));
			if (pthread_key_create(&ti->key, NULL) != 0) errx(1, "no thread-local key");
			if (lo != ~0ULL) { ti->size = hi - lo; ti->init = (const uint8_t *)(lo + im->slide); ti->initsize = initend > lo ? initend - lo : 0; }
		}
		for (k = 0; k < sc->size / 24; k++) {
			uintptr_t *d = (uintptr_t *)(sc->addr + im->slide + k * 24);
			d[0] = (uintptr_t)machrun_tlv_thunk; d[1] = (uintptr_t)ti;		/* d[2]: the offset from the start of the template */
			if (lo != ~0ULL) d[2] = d[2];
		}
	}
}

/* rebase and bind an image after the dylibs that it uses */
static void
bind_image(struct image *im)
{
	int i;
	if (im->bound) return;
	im->bound = true;
	for (i = 0; i < im->nlib; i++) if (im->libimg[i]) bind_image(im->libimg[i]);
	if (im->chained) do_chained(im);
	if (im->dyld_info) {
		const struct dyld_info_command *di = im->dyld_info;
		do_rebase(im);
		do_bind(im, di->bind_off, di->bind_size, false);
		do_bind(im, di->lazy_bind_off, di->lazy_bind_size, true);
	}
	setup_tlv(im);
	/* the image is protected after its Objective-C metadata was set up (objc_load_deps) */
}

/* the Objective-C metadata of the dylibs (libmpv has a little) is set up before that of what uses them */
static void
objc_load_deps(struct image *im, bool is_main)
{
	int i;
	if (im->objc_done) return;
	im->objc_done = true;
	for (i = 0; i < im->nlib; i++) if (im->libimg[i]) objc_load_deps(im->libimg[i], false);
	g_im = im;
	if (objc_load_image2(find_sect, im->slide, is_main ? 1 : 0) < 0) errx(1, "Objective-C setup failed");
	if (!is_main) protect_image(im);
}

/* the initializers of the dylibs run before those of what depends on them */
static void
run_all_initializers(struct image *im, int argc, char **argv, char **envp, char **apple)
{
	int i;
	if (im->inited) return;
	im->inited = true;
	for (i = 0; i < im->nlib; i++) if (im->libimg[i]) run_all_initializers(im->libimg[i], argc, argv, envp, apple);
	VLOG("initializers of %s\n", im->path);
	run_initializers(im, argc, argv, envp, apple);
}

/* ---- library mode: machrun -L LIB.dylib [-L ...] -x SCRIPT: the dylibs are loaded (with what they need), their initializers run, and the script calls functions of them.
 * A line of the script:   [$N =] symbol(arg, arg ...)   where an arg is a number (12, 0x1f), a string "text", a real 2.5, or a result $N of an earlier line; "# ..." is a comment, "sleep N" waits.
 * It is the test of the loader for libraries that no program of this directory uses (libmpv, FFmpeg ...). ---- */
static struct image *
load_lib_file(const char *file)
{
	char real[PATH_MAX]; struct image *d;
	if (realpath(file, real) == NULL) err(1, "%s", file);
	for (d = images; d != NULL; d = d->next) if (strcmp(d->path, real) == 0) return (d);
	d = calloc(1, sizeof(*d)); d->path = strdup(real);
	parse(d, real); map_image(d);
	d->next = images; images = d;
	if (main_image == NULL) main_image = d;
	load_deps(d);
	return (d);
}

static uintptr_t
script_symbol(const char *name)
{
	char buf[256]; struct image *d; uintptr_t a;
	snprintf(buf, sizeof(buf), "_%s", name);
	if (strcmp(name, "peek") == 0 || strcmp(name, "peek32") == 0 || strcmp(name, "peek16") == 0 || strcmp(name, "poke32") == 0 || strcmp(name, "poke") == 0) return (0);
	for (d = images; d != NULL; d = d->next) if ((a = image_export(d, buf, 0)) != 0) { VLOG("%s: %#lx in %s (image at %#lx, slide %#lx)\n", buf, (unsigned long)a, d->path, (unsigned long)(d->min_vm + d->slide), (unsigned long)d->slide); return (a); }
	return ((uintptr_t)dlsym(RTLD_DEFAULT, name));		/* the C library of this system (calloc, printf ...) */
}

static uintptr_t peek_fn(uintptr_t addr, uintptr_t off) { return (*(uintptr_t *)(addr + off)); }
static uintptr_t peek32_fn(uintptr_t addr, uintptr_t off) { return (*(uint32_t *)(addr + off)); }
static uintptr_t peek16_fn(uintptr_t addr, uintptr_t off) { return (*(uint16_t *)(addr + off)); }
static uintptr_t poke32_fn(uintptr_t addr, uintptr_t off, uintptr_t v) { *(uint32_t *)(addr + off) = (uint32_t)v; return (v); }
static uintptr_t poke_fn(uintptr_t addr, uintptr_t off, uintptr_t v) { *(uintptr_t *)(addr + off) = v; return (v); }

static int
run_script(const char *file)
{
	FILE *f = fopen(file, "r"); char line[2048]; uintptr_t var[64]; int failed = 0, lineno = 0;
	memset(var, 0, sizeof(var));
	if (f == NULL) err(1, "%s", file);
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *name, *open, *close; int assign = -1; uintptr_t iv[8] = { 0 }; double dv[4] = { 0 }; int ni = 0, nd = 0; uintptr_t fn, r;
		lineno++;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == 0) continue;
		if (strncmp(p, "sleep ", 6) == 0) { sleep(atoi(p + 6)); continue; }
		if (*p == '$') { assign = (int)strtol(p + 1, &p, 10); while (*p == ' ' || *p == '=') p++; }
		name = p; open = strchr(p, '('); close = strrchr(p, ')');
		if (!open || !close || close < open) { fprintf(stderr, "script:%d: not a call\n", lineno); failed++; continue; }
		*open = 0; *close = 0; p = open + 1;
		while (*p) {
			while (*p == ' ' || *p == ',') p++;
			if (*p == 0) break;
			if (*p == '"') {				/* a string: it stays in memory for the whole run */
				char *e = ++p, *w; w = e;
				while (*e && *e != '"') { if (*e == '\\' && e[1] == 'n') { *w++ = '\n'; e += 2; } else *w++ = *e++; }
				*w = 0; if (ni < 8) iv[ni++] = (uintptr_t)strdup(p); p = *e ? e + 1 : e;
			} else if (*p == '$') { int n = (int)strtol(p + 1, &p, 10); if (ni < 8) iv[ni++] = var[n & 63]; }
			else {
				char *e; double d = strtod(p, &e);
				if (memchr(p, '.', e - p) != NULL) { if (nd < 4) dv[nd++] = d; }
				else if (ni < 8) iv[ni++] = (uintptr_t)strtoll(p, &e, 0);
				p = e;
			}
			if (*p && *p != ',' && *p != ' ') { p++; }
		}
		while (*name == ' ') name++;
		{ char *e = name + strlen(name); while (e > name && (e[-1] == ' ')) *--e = 0; }
		fn = strcmp(name, "peek") == 0 ? (uintptr_t)peek_fn : strcmp(name, "peek32") == 0 ? (uintptr_t)peek32_fn : strcmp(name, "peek16") == 0 ? (uintptr_t)peek16_fn : strcmp(name, "poke32") == 0 ? (uintptr_t)poke32_fn : strcmp(name, "poke") == 0 ? (uintptr_t)poke_fn : script_symbol(name);
		if (fn == 0) { printf("%s: no such symbol\n", name); fflush(stdout); failed++; continue; }
		r = ((uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, double, double, double, double))fn)(iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], dv[0], dv[1], dv[2], dv[3]);
		if (assign >= 0) var[assign & 63] = r;
		printf("%s%s -> %#lx\n", assign >= 0 ? "$" : "", name, (unsigned long)r); fflush(stdout);
		(void)ni;
	}
	fclose(f);
	return (failed);
}

/* the file of the image that holds an address (compat.m: +[NSBundle bundleForClass:] of a class that a plug-in or a framework defines) */
const char *
machrun_image_path_for_addr(const void *addr)
{
	struct image *d;
	for (d = images; d != NULL; d = d->next) if ((uintptr_t)addr >= d->min_vm + d->slide && (uintptr_t)addr < d->max_vm + d->slide) return (d->path);
	if (main_image && (uintptr_t)addr >= main_image->min_vm + main_image->slide && (uintptr_t)addr < main_image->max_vm + main_image->slide) return (main_image->path);
	return (NULL);
}

/* MACHRUN_DEBUG: a fatal signal says which Mach-O images the program counter and the stack point into */
#include <signal.h>
#include <ucontext.h>
static void
fatal_handler(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv; int i; struct image *d;
#if defined(__x86_64__)
	uintptr_t rip = uc->uc_mcontext.mc_rip, *sp = (uintptr_t *)uc->uc_mcontext.mc_rsp;
#else
	uintptr_t rip = 0, *sp = NULL;
#endif
	dprintf(2, "machrun: signal %d at %#lx (fault address %p)\n", sig, (unsigned long)rip, si->si_addr);
	{ Dl_info di; if (dladdr((void *)rip, &di) && di.dli_sname) dprintf(2, "  pc in %s (%s + %#lx)\n", di.dli_fname, di.dli_sname, (unsigned long)(rip - (uintptr_t)di.dli_saddr)); }
#if defined(__x86_64__)
	dprintf(2, "  rdi %#lx rsi %#lx rdx %#lx rcx %#lx\n", (unsigned long)uc->uc_mcontext.mc_rdi, (unsigned long)uc->uc_mcontext.mc_rsi, (unsigned long)uc->uc_mcontext.mc_rdx, (unsigned long)uc->uc_mcontext.mc_rcx);
	dprintf(2, "  rbx %#lx r12 %#lx r13 %#lx r14 %#lx r15 %#lx\n", (unsigned long)uc->uc_mcontext.mc_rbx, (unsigned long)uc->uc_mcontext.mc_r12, (unsigned long)uc->uc_mcontext.mc_r13, (unsigned long)uc->uc_mcontext.mc_r14, (unsigned long)uc->uc_mcontext.mc_r15);
#endif
	for (d = machrun_images_head(); d != NULL; d = d->next) {
		if (rip >= d->min_vm + d->slide && rip < d->max_vm + d->slide) dprintf(2, "  pc in %s + %#lx\n", d->path, (unsigned long)(rip - d->slide - d->min_vm));
	}
	if (sp) for (i = 0; i < 200; i++) for (d = machrun_images_head(); d != NULL; d = d->next)
		if (sp[i] >= d->min_vm + d->slide && sp[i] < d->max_vm + d->slide) { dprintf(2, "  stack[%d]: %s + %#lx\n", i, d->path, (unsigned long)(sp[i] - d->slide - d->min_vm)); break; }
	{	/* frames by the frame pointers (the GNUstep libraries and this system's libobjc keep them): the caller of a message to a bad object */
		void **fp =
#if defined(__x86_64__)
		    (void **)uc->uc_mcontext.mc_rbp;
#else
		    NULL;
#endif
		for (i = 0; i < 40 && fp != NULL && ((uintptr_t)fp & 7) == 0 && (uintptr_t)fp > 0x10000; i++) {
			void *ra = fp[1]; Dl_info di;
			if (ra == NULL) break;
			for (d = machrun_images_head(); d != NULL; d = d->next) if ((uintptr_t)ra >= d->min_vm + d->slide && (uintptr_t)ra < d->max_vm + d->slide) break;
			if (d != NULL) dprintf(2, "  frame[%d] %s + %#lx\n", i, d->path, (unsigned long)((uintptr_t)ra - d->slide - d->min_vm));
			else if (dladdr(ra, &di) && di.dli_fname && !(di.dli_sname == NULL && strstr(di.dli_fname, ".app/"))) dprintf(2, "  frame[%d] %p %s (%s + %#lx)\n", i, ra, di.dli_fname, di.dli_sname ? di.dli_sname : "?", di.dli_sname ? (unsigned long)((uintptr_t)ra - (uintptr_t)di.dli_saddr) : 0UL);
			else dprintf(2, "  frame[%d] %p (no image)\n", i, ra);
			fp = (void **)fp[0];
		}
	}
	{	/* the memory around the registers that look like pointers: written to a pipe (a bad address is an error there, not another fault) and read back */
		static int pfd[2] = { -1, -1 }; unsigned long regs[6]; int r; unsigned char tmp[64];
#if defined(__x86_64__)
		regs[0] = uc->uc_mcontext.mc_rdi; regs[1] = uc->uc_mcontext.mc_rsi; regs[2] = uc->uc_mcontext.mc_rdx; regs[3] = uc->uc_mcontext.mc_rcx; regs[4] = uc->uc_mcontext.mc_rbx; regs[5] = uc->uc_mcontext.mc_r12;
#else
		for (r = 0; r < 6; r++) regs[r] = 0;
#endif
		if (pipe(pfd) == 0) for (r = 0; r < 6; r++) {
			ssize_t n = regs[r] > 0x10000 ? write(pfd[1], (void *)regs[r], 64) : -1;
			if (n == 64 && read(pfd[0], tmp, 64) == 64) { int k; dprintf(2, "  mem[reg%d %#lx]:", r, regs[r]); for (k = 0; k < 64; k++) dprintf(2, " %02x", tmp[k]); dprintf(2, "  |"); for (k = 0; k < 64; k++) dprintf(2, "%c", tmp[k] >= 32 && tmp[k] < 127 ? tmp[k] : '.'); dprintf(2, "|\n"); }
		}
	}
	if (getenv("MACHRUN_SEGV_HOLD")) { dprintf(2, "machrun: holding (pid %d) for procstat\n", (int)getpid()); sleep(90); }
	_exit(139);
}
static void
install_fatal_handler(void)
{
	static char stk[1 << 16]; stack_t ss = { .ss_sp = stk, .ss_size = sizeof(stk), .ss_flags = 0 }; struct sigaction sa;
	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa)); sa.sa_sigaction = fatal_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}

/* A Mach-O bundle (a plug-in) that the program asks to load: it is loaded like a dylib, after the program has started */
/* is there code for this machine in the file? (a plug-in for another architecture is no reason to end the program) */
static bool
macho_for_this_machine(const char *path)
{
	uint8_t b[8 + 20 * 16]; int fd = open(path, O_RDONLY); ssize_t n; bool ok = false; uint32_t i;
	const uint32_t hostcpu =
#if defined(__x86_64__)
	    CPU_TYPE_X86_64;
#else
	    CPU_TYPE_ARM64;
#endif
	if (fd < 0) return (false);
	n = read(fd, b, sizeof(b)); close(fd);
	if (n < 8) return (false);
	if (be32dec(b) == FAT_MAGIC) { uint32_t cnt = be32dec(b + 4); for (i = 0; i < cnt && i < 16 && 8 + 20 * (i + 1) <= (uint32_t)n; i++) if (be32dec(b + 8 + 20 * i) == hostcpu) ok = true; }
	else if (le32dec(b) == MH_MAGIC_64 && le32dec(b + 4) == hostcpu) ok = true;
	return (ok);
}

void *
machrun_load_macho(const char *path)
{
	struct image *d, *k; char *apple2[2] = { NULL, NULL }; char *none[1] = { NULL };
	if (!macho_for_this_machine(path)) { VLOG("%s: no code for this machine\n", path); return (NULL); }
	d = load_lib_file(path);
	for (k = images; k != NULL; k = k->next) bind_image(k);
	for (k = images; k != NULL; k = k->next) objc_load_deps(k, false);
	objc_call_loads();
	for (k = images; k != NULL; k = k->next) run_all_initializers(k, 0, none, environ, apple2);
	return (d);
}

int
main(int argc, char **argv)
{
	struct image im; int ch = 1, ret; char *apple[2]; char path[PATH_MAX + 16];
	extern char **environ;

	if (getenv("MACHRUN_DEBUG")) machrun_verbose = 1;
	if (getenv("MACHRUN_SEGV")) install_fatal_handler();
	{	/* launcher mode: started under another name (e.g. as Keka.app/Contents/MacOS/Keka), it runs the Mach-O file next to it,
		 * NAME.macho, so that the process, and with it the main bundle of the program, is the application's own */
		const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
		if (strcmp(base, "machrun") != 0) {
			static char real[PATH_MAX + 8]; char self[PATH_MAX]; size_t n = sizeof(self);
			int mib[4] = { 1 /* CTL_KERN */, 14 /* KERN_PROC */, 12 /* KERN_PROC_PATHNAME */, -1 };
			if (sysctl(mib, 4, self, &n, NULL, 0) != 0) strlcpy(self, argv[0], sizeof(self));
			snprintf(real, sizeof(real), "%s.macho", self);
			if (access(real, R_OK) != 0) errx(66, "launcher mode: %s not found", real);
			launcher_real = real;
			ch = 0;					/* argv[0] is the program's own name, the arguments follow */
		}
	}
	{	/* library mode */
		int k; const char *script = NULL; int nl = 0;
		for (k = 1; k < argc; k++) { if (strcmp(argv[k], "-L") == 0 && k + 1 < argc) nl++; if (strcmp(argv[k], "-x") == 0 && k + 1 < argc) script = argv[k + 1]; if (strcmp(argv[k], "-v") == 0) machrun_verbose = 1; }
		if (nl > 0) {
			lib_mode = true;
			char *apple2[2] = { NULL, NULL }; struct image *d; int fails;
			static char selfpath[PATH_MAX]; size_t sl = sizeof(selfpath); int smib[4] = { 1, 14, 12, -1 };
			char *fake[2] = { selfpath, NULL };
			if (sysctl(smib, 4, selfpath, &sl, NULL, 0) != 0) strlcpy(selfpath, argv[0], sizeof(selfpath));
			for (k = 1; k < argc; k++) if (strcmp(argv[k], "-L") == 0 && k + 1 < argc) load_lib_file(argv[++k]);
			{ void (*gsargs)(int, char **, char **) = dlsym(RTLD_DEFAULT, "GSInitializeProcess"); if (gsargs) gsargs(1, fake, environ); }
			shim_set_process(1, fake, environ, argv[0]);
			for (d = images; d != NULL; d = d->next) bind_image(d);
			VLOG("%u unresolved symbols\n", unresolved_count);
			for (d = images; d != NULL; d = d->next) objc_load_deps(d, false);
			objc_call_loads();
			for (d = images; d != NULL; d = d->next) run_all_initializers(d, 1, fake, environ, apple2);
			fails = script ? run_script(script) : 0;
			fflush(stdout);
			exit(fails ? 1 : 0);
		}
	}
	while (launcher_real == NULL && ch < argc && argv[ch][0] == '-') {
		if (strcmp(argv[ch], "-v") == 0) machrun_verbose = 1;
		else if (strcmp(argv[ch], "--") == 0) { ch++; break; }
		else errx(64, "usage: machrun [-v] PROGRAM [arguments]");
		ch++;
	}
	if (launcher_real == NULL && ch >= argc) errx(64, "usage: machrun [-v] PROGRAM [arguments]");
	memset(&im, 0, sizeof(im));
	parse(&im, launcher_real ? launcher_real : argv[ch]);
	if (!im.have_main && !im.have_thread) errx(1, "%s: no LC_MAIN and no LC_UNIXTHREAD: no entry point", launcher_real ? launcher_real : argv[ch]);
	{	/* an Objective-C program (a Mac application): the settings that made them run in the tests.  MALLOC_CONF=junk:true has to be in the environment before the allocator
		 * starts, so the process starts itself again once; MACHRUN_STRICT=1 turns the leniency off (messages that nothing implements raise as GNUstep's do) */
		int objc = 0, i; uint32_t j;
		for (i = 0; i < im.nseg; i++) for (j = 0; j < im.seg[i].nsects; j++) if (strncmp(im.seg[i].sects[j].sectname, "__objc_classlist", 16) == 0) objc = 1;
		if (objc && getenv("MACHRUN_STRICT") == NULL) setenv("MACHRUN_LENIENT", "1", 0);
		if (objc && getenv("MALLOC_CONF") == NULL && getenv("MACHRUN_NOREEXEC") == NULL) {
			char self[PATH_MAX]; size_t n = sizeof(self); int mib[4] = { 1, 14, 12, -1 };
			setenv("MALLOC_CONF", "junk:true", 1);
			if (sysctl(mib, 4, self, &n, NULL, 0) == 0) { execv(self, argv); }
		}
	}
	{ char rp[PATH_MAX]; im.path = strdup(realpath(launcher_real ? launcher_real : argv[ch], rp) ? rp : argv[ch]); }
	main_image = &im;
	map_image(&im);
	load_deps(&im);
	{	/* GNUstep wants to see the arguments and the environment of the process (its own main() macro does this) */
		void (*gsargs)(int, char **, char **) = dlsym(RTLD_DEFAULT, "GSInitializeProcess");
		if (gsargs) { VLOG("GNUstep: GSInitializeProcess\n"); gsargs(argc - ch, argv + ch, environ); }
	}
	bind_image(&im);
	VLOG("%u unresolved symbols\n", unresolved_count);
	if (getenv("MACHRUN_DUMPGOT")) { extern void machrun_objc_msgSend(void); VLOG("got slot %p (machrun_objc_msgSend %p)\n", *(void **)(im.slide + strtoul(getenv("MACHRUN_DUMPGOT"), NULL, 0)), (void *)machrun_objc_msgSend); }
	if (realpath(launcher_real ? launcher_real : argv[ch], path) == NULL) strlcpy(path, argv[ch], sizeof(path));
	{ char *ap = malloc(strlen(path) + 32); snprintf(ap, strlen(path) + 32, "executable_path=%s", path); apple[0] = ap; apple[1] = NULL; }
	shim_set_process(argc - ch, argv + ch, environ, path);
	objc_load_deps(&im, true);
	protect_image(&im);

	objc_call_loads();
	run_all_initializers(&im, argc - ch, argv + ch, environ, apple);
	if (!im.have_main) {			/* an older program (LC_UNIXTHREAD): its start function expects the stack that the kernel makes: argc, argv, envp, apple */
		int ac = argc - ch, ne = 0, k; char **av = argv + ch; size_t words, sz = 8 << 20; uintptr_t *sp; char *stk;
		while (environ[ne]) ne++;
		stk = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_STACK, -1, 0);
		if (stk == MAP_FAILED) err(1, "mmap stack");
		words = 1 + ac + 1 + ne + 1 + 1 + 1;
		sp = (uintptr_t *)(stk + sz - 4096 - words * 8); sp = (uintptr_t *)((uintptr_t)sp & ~(uintptr_t)15);
		{ uintptr_t *w = sp; *w++ = (uintptr_t)ac; for (k = 0; k < ac; k++) *w++ = (uintptr_t)av[k]; *w++ = 0; for (k = 0; k < ne; k++) *w++ = (uintptr_t)environ[k]; *w++ = 0; *w++ = (uintptr_t)apple[0]; *w++ = 0; }
		VLOG("start at %p (LC_UNIXTHREAD), stack %p\n", (void *)(im.thread_pc + im.slide), (void *)sp);
#if defined(__x86_64__)
		__asm__ volatile("movq %0,%%rsp\n\txorl %%ebp,%%ebp\n\tjmp *%1" :: "r"(sp), "r"(im.thread_pc + im.slide) : "memory");
#endif
		_exit(127);
	}
	VLOG("main at %p\n", (void *)(im.text_vm + im.slide + im.entryoff));
	ret = ((int (*)(int, char **, char **, char **))(im.text_vm + im.slide + im.entryoff))(argc - ch, argv + ch, environ, apple);
	exit(ret);
}
