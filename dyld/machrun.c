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
	uint64_t	min_vm, max_vm;		/* preferred addresses of the mapped part */
	uint64_t	text_vm;		/* preferred address of __TEXT: LC_MAIN's offset is from here */
	intptr_t	slide;
	uint64_t	entryoff;
	bool		have_main;
	uint64_t	thread_pc;
	bool		have_thread;
	uint32_t	flags;
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
	if (mh->magic != MH_MAGIC_64 || mh->cputype != hostcpu || mh->filetype != MH_EXECUTE)
		errx(1, "%s: not a Mach-O executable for this architecture", path);
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
			im->dyld_info = (const void *)lc; break;
		case LC_DYLD_CHAINED_FIXUPS:
			im->chained = true; break;
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
	void *a = mmap((void *)im->min_vm, span, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_EXCL, -1, 0);
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

static void
open_libs(struct image *im)
{
	int i;
	for (i = 0; i < im->nlib; i++) {
		im->libhandle[i] = shim_open_library(im->libname[i]);
		VLOG("library %d: %s\n", i + 1, im->libname[i]);
	}
}

static uintptr_t
resolve(struct image *im, int ordinal, const char *name, bool weak)
{
	void *h = NULL, *a;
	if (ordinal >= 1 && ordinal <= im->nlib) h = im->libhandle[ordinal - 1];
	a = shim_lookup(h, name);
	if (strncmp(name, "_CF", 3) == 0) VLOG("bind %s -> %p\n", name, a);
	if (a == NULL) {
		if (weak) return (0);
		unresolved_count++;
		VLOG("UNRESOLVED %s\n", name);
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
				VLOG("warning: the program has thread-local variables (not supported yet)\n");
		}
	}
}

int
main(int argc, char **argv)
{
	struct image im; int ch = 1, ret; char *apple[2]; char path[PATH_MAX + 16];
	extern char **environ;

	if (getenv("MACHRUN_DEBUG")) machrun_verbose = 1;
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
	while (launcher_real == NULL && ch < argc && argv[ch][0] == '-') {
		if (strcmp(argv[ch], "-v") == 0) machrun_verbose = 1;
		else if (strcmp(argv[ch], "--") == 0) { ch++; break; }
		else errx(64, "usage: machrun [-v] PROGRAM [arguments]");
		ch++;
	}
	if (launcher_real == NULL && ch >= argc) errx(64, "usage: machrun [-v] PROGRAM [arguments]");
	memset(&im, 0, sizeof(im));
	parse(&im, launcher_real ? launcher_real : argv[ch]);
	if (im.chained) errx(1, "%s: chained fixups (LC_DYLD_CHAINED_FIXUPS) are not supported yet", launcher_real ? launcher_real : argv[ch]);
	if (!im.have_main) errx(1, "%s: no LC_MAIN (only programs that start at main() are supported)", launcher_real ? launcher_real : argv[ch]);
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
	map_image(&im);
	open_libs(&im);
	{	/* GNUstep wants to see the arguments and the environment of the process (its own main() macro does this) */
		void (*gsargs)(int, char **, char **) = dlsym(RTLD_DEFAULT, "GSInitializeProcess");
		if (gsargs) { VLOG("GNUstep: GSInitializeProcess\n"); gsargs(argc - ch, argv + ch, environ); }
	}
	if (im.dyld_info) {
		const struct dyld_info_command *di = im.dyld_info;
		do_rebase(&im);
		do_bind(&im, di->bind_off, di->bind_size, false);
		do_bind(&im, di->lazy_bind_off, di->lazy_bind_size, true);
	}
	VLOG("%u unresolved symbols\n", unresolved_count);
	if (getenv("MACHRUN_DUMPGOT")) VLOG("got CFRelease slot %p\n", *(void **)(im.slide + 0x100103198));
	if (realpath(launcher_real ? launcher_real : argv[ch], path) == NULL) strlcpy(path, argv[ch], sizeof(path));
	{ char *ap = malloc(strlen(path) + 32); snprintf(ap, strlen(path) + 32, "executable_path=%s", path); apple[0] = ap; apple[1] = NULL; }
	shim_set_process(argc - ch, argv + ch, environ, path);
	g_im = &im;
	if (objc_load_image(find_sect, im.slide) < 0) errx(1, "Objective-C setup failed");
	protect_image(&im);

	objc_call_loads();
	run_initializers(&im, argc - ch, argv + ch, environ, apple);
	VLOG("main at %p\n", (void *)(im.text_vm + im.slide + im.entryoff));
	ret = ((int (*)(int, char **, char **, char **))(im.text_vm + im.slide + im.entryoff))(argc - ch, argv + ch, environ, apple);
	exit(ret);
}
