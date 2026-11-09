/*
 * The Mach-O image activator.  Handles thin and fat (universal) 64 bit files for the architecture of the
 * running kernel: LC_SEGMENT_64 segments are mapped, the entry point comes from LC_MAIN or LC_UNIXTHREAD,
 * and the process gets the Darwin system call vector.
 *
 * Files that name a dynamic linker (almost every Mac program) are not handled here: the activator answers "not mine" and imgact_binmisc can hand them to dyld/machrun:
 *
 * Like imgact_elf.c, the activator returns -1 for a file that is not a Mach-O file, so that the other
 * activators get their turn, and an error number for a file that is one but cannot be run.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/exec.h>
#include <sys/imgact.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/sysent.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>
#if defined(__aarch64__)
#include <machine/cpufunc.h>
#endif

#include "darwinator.h"
#include "macho.h"

#if defined(__amd64__)
#define	HOST_CPUTYPE	CPU_TYPE_X86_64
#elif defined(__aarch64__)
#define	HOST_CPUTYPE	CPU_TYPE_ARM64
#else
#error "darwinator: unsupported architecture"
#endif

#define	MACHO_MAXCMDS	(64 * 1024)		/* largest load command area accepted */
#define	MACHO_MAXSEGS	16
#define	MACHO_CHUNK	(64 * 1024)		/* size of one read when a segment is copied in */

struct macho_seg {
	vm_offset_t	start, end;		/* page aligned range in memory */
	uint64_t	vmaddr, fileoff, filesize;
	vm_prot_t	prot;
};

struct macho_info {
	off_t		base;			/* offset of the Mach-O image in the file (non zero for a fat file) */
	off_t		size;			/* size of the image */
	struct macho_seg seg[MACHO_MAXSEGS];
	int		nseg;
	uint64_t	text_vmaddr;
	uint64_t	entry;
	bool		have_entry;
	bool		needs_dyld;
	bool		has_dylinker;
	char		dyld_path[128];		/* LC_LOAD_DYLINKER */
	struct vnode	*vp;			/* the file that is read (the executable, or dyld) */
	off_t		fsize;
	uint32_t	want_filetype;		/* MH_EXECUTE, or MH_DYLINKER for dyld */
	uint64_t	slide;			/* added to every address of the image (dyld is placed by the kernel) */
};

static int
macho_read(struct vnode *vp, void *buf, size_t len, off_t off)
{
	ssize_t resid;
	int error;

	error = vn_rdwr(UIO_READ, vp, buf, len, off, UIO_SYSSPACE, IO_NODELOCKED,
	    curthread->td_ucred, NOCRED, &resid, curthread);
	if (error == 0 && resid != 0)
		error = ENOEXEC;		/* the file is shorter than its headers say */
	return (error);
}

/* malloc with the vnode locked: when the allocation would sleep, the vnode is unlocked meanwhile (as imgact_elf does) */
static void *
macho_alloc(struct image_params *imgp, size_t len)
{
	void *p = malloc(len, M_DARWIN, M_NOWAIT);

	if (p == NULL) {
		VOP_UNLOCK(imgp->vp);
		p = malloc(len, M_DARWIN, M_WAITOK);
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
	}
	return (p);
}

static vm_prot_t
macho_prot(uint32_t p)
{
	return (((p & VMP_READ) ? VM_PROT_READ : 0) | ((p & VMP_WRITE) ? VM_PROT_WRITE : 0) |
	    ((p & VMP_EXEC) ? VM_PROT_EXECUTE : 0));
}

/* chooses the slice of a fat file that is for this architecture */
static int
macho_fat_slice(struct image_params *imgp, struct macho_info *mi)
{
	struct fat_header fh;
	struct fat_arch fa;
	uint32_t n, i;
	int error, best = -1;
	struct fat_arch bestfa = { 0 };

	error = macho_read(mi->vp, &fh, sizeof(fh), 0);
	if (error != 0)
		return (error);
	n = be32toh(fh.nfat_arch);
	if (n == 0 || n > 16)
		return (ENOEXEC);
	for (i = 0; i < n; i++) {
		error = macho_read(mi->vp, &fa, sizeof(fa), sizeof(fh) + (off_t)i * sizeof(fa));
		if (error != 0)
			return (error);
		if (be32toh(fa.cputype) != HOST_CPUTYPE)
			continue;
		/* the plain subtype (arm64, x86_64) is preferred to arm64e / x86_64h */
		if (best < 0 || (be32toh(fa.cpusubtype) & 0xff) == 0 || (be32toh(fa.cpusubtype) & 0xff) == 3) {
			best = i;
			bestfa = fa;
		}
	}
	if (best < 0) {
		DPRINTF("fat file without a slice for this architecture\n");
		return (ENOEXEC);
	}
	mi->base = be32toh(bestfa.offset);
	mi->size = be32toh(bestfa.size);
	if (mi->base + mi->size > mi->fsize || mi->size < (off_t)sizeof(struct mach_header_64))
		return (ENOEXEC);
	return (0);
}

/* reads and checks the headers; fills in mi, no change is made to the process */
static int
macho_parse(struct image_params *imgp, struct macho_info *mi)
{
	struct mach_header_64 mh;
	struct load_command *lc;
	char *cmds = NULL;
	uint32_t i, off, magic_be;
	int error;
	bool have_main = false, have_thread = false;
	uint64_t main_off = 0, thread_pc = 0;

	{
		struct vnode *vp = mi->vp;
		off_t fsize = mi->fsize;
		uint32_t want = mi->want_filetype;
		uint64_t slide = mi->slide;
		uint32_t m0;

		memset(mi, 0, sizeof(*mi));
		mi->vp = vp; mi->fsize = fsize; mi->want_filetype = want; mi->slide = slide;
		mi->size = fsize;
		error = macho_read(vp, &m0, sizeof(m0), 0);
		if (error != 0)
			return (error);
		magic_be = be32toh(m0);
	}
	if (magic_be == FAT_MAGIC) {
		error = macho_fat_slice(imgp, mi);
		if (error != 0)
			return (error);
	}
	error = macho_read(mi->vp, &mh, sizeof(mh), mi->base);
	if (error != 0)
		return (error);
	if (mh.magic != MH_MAGIC_64)
		return (ENOEXEC);
	if (mh.cputype != HOST_CPUTYPE) {
		DPRINTF("cputype %#x is not this architecture\n", mh.cputype);
		return (ENOEXEC);
	}
	if (mh.filetype != mi->want_filetype) {
		DPRINTF("filetype %u is not an executable\n", mh.filetype);
		return (ENOEXEC);
	}
	if (mh.sizeofcmds > MACHO_MAXCMDS || (uint64_t)mh.ncmds * sizeof(*lc) > mh.sizeofcmds ||
	    sizeof(mh) + (uint64_t)mh.sizeofcmds > (uint64_t)mi->size)
		return (ENOEXEC);

	cmds = macho_alloc(imgp, mh.sizeofcmds ? mh.sizeofcmds : 1);
	error = macho_read(mi->vp, cmds, mh.sizeofcmds, mi->base + sizeof(mh));
	if (error != 0)
		goto out;

	error = ENOEXEC;
	for (i = 0, off = 0; i < mh.ncmds; i++) {
		if (off + sizeof(*lc) > mh.sizeofcmds)
			goto out;
		lc = (struct load_command *)(cmds + off);
		if (lc->cmdsize < sizeof(*lc) || (lc->cmdsize & 7) != 0 || lc->cmdsize > mh.sizeofcmds - off)
			goto out;

		switch (lc->cmd) {
		case LC_SEGMENT_64: {
			struct segment_command_64 *sc = (struct segment_command_64 *)lc;
			struct macho_seg *s;
			uint64_t end, va;

			if (lc->cmdsize < sizeof(*sc))
				goto out;
			if (strncmp(sc->segname, "__PAGEZERO", sizeof(sc->segname)) == 0)
				break;			/* the unmapped bottom of the address space */
			if (sc->vmsize == 0)
				break;
			if (mi->nseg == MACHO_MAXSEGS || ((sc->vmaddr + mi->slide) & PAGE_MASK) != 0 ||
			    sc->filesize > sc->vmsize || sc->fileoff + sc->filesize < sc->fileoff ||
			    sc->fileoff + sc->filesize > (uint64_t)mi->size)
				goto out;
			va = sc->vmaddr + mi->slide;
			end = va + sc->vmsize;
			if (end < va || va < PAGE_SIZE ||
			    end > darwin_sysvec.sv_usrstack - maxssiz)
				goto out;
			s = &mi->seg[mi->nseg++];
			s->vmaddr = va;
			s->start = trunc_page(va);
			s->end = round_page(end);
			s->fileoff = sc->fileoff;
			s->filesize = sc->filesize;
			s->prot = macho_prot(sc->initprot);
			if (strncmp(sc->segname, "__TEXT", sizeof(sc->segname)) == 0)
				mi->text_vmaddr = va;
			break;
		}
		case LC_MAIN:
			if (lc->cmdsize < sizeof(struct entry_point_command))
				goto out;
			have_main = true;
			main_off = ((struct entry_point_command *)lc)->entryoff;
			break;
		case LC_UNIXTHREAD: {
			struct thread_command *tc = (struct thread_command *)lc;
			const uint32_t *state = (const uint32_t *)(tc + 1);
			uint32_t flavor, count, pc_index;

			if (lc->cmdsize < sizeof(*tc))
				goto out;
#if defined(__amd64__)
			flavor = x86_THREAD_STATE64; count = x86_THREAD_STATE64_COUNT; pc_index = X86_STATE64_RIP_INDEX;
#else
			flavor = ARM_THREAD_STATE64; count = ARM_THREAD_STATE64_COUNT; pc_index = ARM_STATE64_PC_INDEX;
#endif
			if (tc->flavor != flavor || tc->count != count ||
			    lc->cmdsize < sizeof(*tc) + count * sizeof(uint32_t))
				goto out;
			have_thread = true;
			memcpy(&thread_pc, state + 2 * pc_index, sizeof(thread_pc));
			break;
		}
		case LC_LOAD_DYLINKER:
		case LC_LOAD_DYLIB: {
			/* both commands start with cmd, cmdsize, and the offset of the name (a dylib_command has more after it) */
			uint32_t noff = lc->cmdsize >= 12 ? ((const uint32_t *)lc)[2] : 0;

			mi->needs_dyld = true;
			if (lc->cmd == LC_LOAD_DYLINKER && noff >= 12 && noff < lc->cmdsize) {
				mi->has_dylinker = true;
				strlcpy(mi->dyld_path, (const char *)lc + noff, MIN(sizeof(mi->dyld_path), lc->cmdsize - noff + 1));
			}
			if (noff >= 12 && noff < lc->cmdsize)
				DPRINTF("  needs %s %.*s\n", lc->cmd == LC_LOAD_DYLINKER ? "linker" : "library",
				    (int)(lc->cmdsize - noff), (const char *)lc + noff);
			break;
		}
		default:
			break;
		}
		off += lc->cmdsize;
	}

	if (mi->nseg == 0 || (!have_main && !have_thread))
		goto out;
	if (have_main) {
		if (mi->text_vmaddr == 0)
			goto out;
		mi->entry = mi->text_vmaddr + main_off;
	} else
		mi->entry = thread_pc + mi->slide;
	/* the entry point has to be in an executable segment */
	for (i = 0; i < (uint32_t)mi->nseg; i++)
		if (mi->entry >= mi->seg[i].vmaddr && mi->entry < mi->seg[i].end && (mi->seg[i].prot & VM_PROT_EXECUTE))
			mi->have_entry = true;
	error = mi->have_entry ? 0 : ENOEXEC;
out:
	free(cmds, M_DARWIN);
	if (error != 0)
		DPRINTF("malformed Mach-O headers\n");
	return (error);
}

/* maps one segment: zero filled memory, then the bytes of the file, then the final protection */
static int
macho_map_segment(struct image_params *imgp, vm_map_t map, const struct macho_info *mi,
    const struct macho_seg *s, void *buf)
{
	uint64_t done, n;
	int error, rv;

	rv = vm_map_fixed(map, NULL, 0, s->start, s->end - s->start, VM_PROT_RW, VM_PROT_ALL, MAP_CHECK_EXCL);
	if (rv != KERN_SUCCESS) {
		DPRINTF("vm_map_fixed %#lx-%#lx failed: %d\n", (u_long)s->start, (u_long)s->end, rv);
		return (vm_mmap_to_errno(rv));
	}

	for (done = 0; done < s->filesize; done += n) {
		n = MIN(s->filesize - done, MACHO_CHUNK);
		error = macho_read(mi->vp, buf, n, mi->base + s->fileoff + done);
		if (error == 0)
			error = copyout(buf, (void *)(uintptr_t)(s->vmaddr + done), n);
		if (error != 0) {
			DPRINTF("segment copy at %#lx failed: %d\n", (u_long)(s->vmaddr + done), error);
			return (error);
		}
	}
#if defined(__aarch64__)
	/* the code was written with copyout: the instruction cache has to see it (the pages are not coming from a vnode, so the kernel does no sync of its own) */
	if (s->prot & VM_PROT_EXECUTE)
		(void)cpu_icache_sync_range_checked(s->vmaddr, s->filesize);
#endif
	if (s->prot != VM_PROT_RW) {
		rv = vm_map_protect(map, s->start, s->end, s->prot, 0, VM_MAP_PROTECT_SET_PROT);
		if (rv != KERN_SUCCESS) {
			DPRINTF("vm_map_protect failed: %d\n", rv);
			return (vm_mmap_to_errno(rv));
		}
	}
	return (0);
}

#define	DARWIN_DYLD_BASE	0x7ff700000000ULL	/* where dyld is placed: below the shared region (0x7ff800000000) */

/* opens a file for reading (the way imgact_elf opens its interpreter); the vnode comes back locked shared and referenced, with its size */
static int
macho_open_file(struct image_params *imgp, const char *path, struct vnode **vpp, off_t *sizep)
{
	struct nameidata nd;
	struct vattr va;
	int error;

	NDINIT(&nd, LOOKUP, FOLLOW | LOCKSHARED | LOCKLEAF, UIO_SYSSPACE, path);
	error = namei(&nd);
	if (error != 0)
		return (error);
	NDFREE_PNBUF(&nd);
	if (nd.ni_vp->v_type != VREG) {
		vput(nd.ni_vp);
		return (ENOEXEC);
	}
	error = VOP_GETATTR(nd.ni_vp, &va, curthread->td_ucred);
	if (error == 0)
		error = VOP_ACCESS(nd.ni_vp, VREAD, curthread->td_ucred, curthread);
	if (error == 0)
		error = VOP_OPEN(nd.ni_vp, FREAD, curthread->td_ucred, curthread, NULL);
	if (error != 0) {
		vput(nd.ni_vp);
		return (error);
	}
	*vpp = nd.ni_vp;
	*sizep = va.va_size;
	return (0);
}

/* the strings of the "apple" vector that dyld reads: appended to the environment strings and hidden from the environment (envc is taken back) */
static void
macho_add_apple(struct image_params *imgp, struct darwin_auxargs *aa)
{
	char str[1100];
	uint64_t r1, r2, r3, r4;
	int i;

	arc4rand(&r1, sizeof(r1), 0); arc4rand(&r2, sizeof(r2), 0); arc4rand(&r3, sizeof(r3), 0); arc4rand(&r4, sizeof(r4), 0);
	for (i = 0; i < 5; i++) {
		switch (i) {
		case 0: snprintf(str, sizeof(str), "executable_path=%s", imgp->execpath != NULL ? imgp->execpath : ""); break;
		case 1: snprintf(str, sizeof(str), "ptr_munge=0x%016lx", (u_long)r1 & ~0xfful); break;
		case 2: snprintf(str, sizeof(str), "stack_guard=0x%016lx", (u_long)r2 & ~0xfful); break;
		case 3: snprintf(str, sizeof(str), "malloc_entropy=0x%016lx,0x%016lx", (u_long)r3, (u_long)r4); break;
		default: snprintf(str, sizeof(str), "th_port=0x103"); break;
		}
		aa->off[aa->n] = (uint32_t)(imgp->args->endp - imgp->args->begin_argv);
		if (exec_args_add_env(imgp->args, str, UIO_SYSSPACE) != 0)
			break;
		imgp->args->envc--;			/* it is in the buffer, and not in the environment */
		aa->n++;
	}
}

int
macho_imgact(struct image_params *imgp)
{
	struct macho_info *mi, *dmi = NULL;
	struct vmspace *vmspace;
	struct vnode *dvp = NULL;
	void *buf;
	uint32_t magic;
	off_t dsize = 0;
	int error, i;

	if (imgp->attr->va_size < sizeof(struct mach_header_64))
		return (-1);
	magic = *(const uint32_t *)imgp->image_header;
	if (magic != MH_MAGIC_64 && be32toh(magic) != FAT_MAGIC)
		return (-1);			/* not ours */

	mi = macho_alloc(imgp, sizeof(*mi));
	buf = NULL;
	mi->vp = imgp->vp; mi->fsize = imgp->attr->va_size; mi->want_filetype = MH_EXECUTE; mi->slide = 0;
	error = macho_parse(imgp, mi);
	if (error != 0)
		goto out;
	if (mi->needs_dyld) {
		if (!darwin_dyld || !mi->has_dylinker) {
			DPRINTF("%s: refused, it needs dyld and its libraries\n", imgp->execpath != NULL ? imgp->execpath : "?");
			/* not ours: -1 lets the next activator try; with imgact_binmisc set up for Mach-O files (binmiscctl add ... --interpreter machrun) the file runs
			 * through machrun, otherwise the exec fails with ENOEXEC as for any unknown format */
			error = -1;
			goto out;
		}
		/* kern.darwinator.dyld: the linker that the file names (the dyld of a Mac) is mapped, and it starts first */
		error = macho_open_file(imgp, mi->dyld_path, &dvp, &dsize);
		if (error != 0) {
			DPRINTF("%s: cannot open %s: %d\n", imgp->execpath != NULL ? imgp->execpath : "?", mi->dyld_path, error);
			goto out;
		}
		dmi = macho_alloc(imgp, sizeof(*dmi));
		dmi->vp = dvp; dmi->fsize = dsize; dmi->want_filetype = MH_DYLINKER; dmi->slide = DARWIN_DYLD_BASE;
		error = macho_parse(imgp, dmi);
		if (error != 0) {
			DPRINTF("%s: %s is not a usable dyld\n", imgp->execpath != NULL ? imgp->execpath : "?", mi->dyld_path);
			goto out;
		}
	}

	DPRINTF("%s: %d segments, entry %#lx%s\n", imgp->execpath != NULL ? imgp->execpath : "?", mi->nseg, (u_long)mi->entry, dmi != NULL ? " (through dyld)" : "");
	error = exec_new_vmspace(imgp, &darwin_sysvec);
	/* as imgact_elf does, whatever the result: the address space is the new one, the process is a Darwin process */
	imgp->proc->p_sysent = &darwin_sysvec;
	imgp->proc->p_elf_brandinfo = NULL;
	if (error != 0) {
		DPRINTF("exec_new_vmspace failed: %d\n", error);
		goto out;
	}
	vmspace = imgp->proc->p_vmspace;

	buf = macho_alloc(imgp, MACHO_CHUNK);
	for (i = 0; i < mi->nseg; i++) {
		error = macho_map_segment(imgp, &vmspace->vm_map, mi, &mi->seg[i], buf);
		if (error != 0)
			goto out;
	}
	if (dmi != NULL) {
		for (i = 0; i < dmi->nseg; i++) {
			error = macho_map_segment(imgp, &vmspace->vm_map, dmi, &dmi->seg[i], buf);
			if (error != 0)
				goto out;
		}
	}
	error = exec_map_stack(imgp);
	if (error != 0) {
		DPRINTF("exec_map_stack failed: %d\n", error);
		goto out;
	}
	error = darwin_commpage_map(vmspace);
	if (error != 0) {
		DPRINTF("the comm page: %d\n", error);
		goto out;
	}

	if (dmi != NULL) {
		struct darwin_auxargs *aa = malloc(sizeof(*aa), M_TEMP, M_NOWAIT | M_ZERO);

		if (aa == NULL) {
			error = ENOMEM;
			goto out;
		}
		aa->mh = mi->text_vmaddr;			/* the mach_header of the program is the start of its __TEXT segment */
		macho_add_apple(imgp, aa);
		imgp->auxargs = aa;			/* freed by do_execve; the stack gets the mach_header, then argc, argv, envp, apple */
		imgp->entry_addr = dmi->entry;
	} else {
		imgp->auxargs = NULL;			/* the stack has argc, argv, envp: no ELF auxiliary vector */
		imgp->entry_addr = mi->entry;
	}
	imgp->reloc_base = mi->text_vmaddr;
	imgp->interpreted = 0;
	imgp->proc->p_osrel = osreldate;
	imgp->proc->p_fctl0 = 0;
	imgp->proc->p_elf_flags = 0;
out:
	if (dvp != NULL) {
		VOP_UNLOCK(dvp);
		vn_close(dvp, FREAD, curthread->td_ucred, curthread);
	}
	free(buf, M_DARWIN);
	free(dmi, M_DARWIN);
	free(mi, M_DARWIN);
	return (error);
}
