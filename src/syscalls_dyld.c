/*
 * The shared region of the dyld cache (what dyld asks the kernel for): shared_region_check_np (294) says where the cache is mapped in this process, and
 * shared_region_map_and_slide_2_np (536) maps the files of the cache at the addresses that dyld gives.  On a Mac the region is a submap that all the processes share;
 * here every process maps the files itself (private, copy on write: the pages come from the same page cache), at the address that dyld gives plus the slide (the random offset of the
 * cache), and the pointers of the data mappings are moved by the slide with the slide info of version 2 (the chains that the cache lists).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/uio.h>
#include <sys/sx.h>
#include <sys/rwlock.h>
#include <sys/sysctl.h>
#include <sys/ucred.h>
#include <sys/sysproto.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

struct sfile { int32_t fd; uint32_t count; uint32_t slide; };		/* shared_file_np */
struct smap { uint64_t addr, size, off, slide_size, slide_start; uint32_t max_prot, init_prot; };	/* shared_file_mapping_slide_np */
_Static_assert(sizeof(struct sfile) == 12, "shared_file_np");
_Static_assert(sizeof(struct smap) == 48, "shared_file_mapping_slide_np");

#define	SMAP_COW	0x08
#define	SMAP_ZF		0x10
#define	SMAP_SLIDE	0x20

#define	SR_N	1024
static struct { struct vmspace *vms; uint64_t start; bool attached; } sr_tab[SR_N];		/* attached: the shared cache of the first process is mapped in this address space (a later check without a start does not map it again) */
static struct mtx sr_mtx;
static bool sr_inited;

/*
 * Sharing the cache between the processes.  The first process that maps the cache makes, for every writable mapping, an object of its own (anonymous memory) with the bytes of the
 * mapping after the slide, and maps that object copy on write instead of the file; for the other mappings it keeps the object of the mapping.  Every later process gets the same
 * objects from shared_region_check_np (the region is "already mapped", as on a Mac, where it is a submap that all share): the pages that nobody writes exist once.  Without this
 * every process rebased ~70 MB of data pages for itself.  The slide is the same in all (the cache is at the same address in every process).
 */
#define	SRG_N	64
static struct srg_map { uint64_t va, size; vm_prot_t prot, maxprot; vm_object_t obj; vm_ooffset_t off; } srg_m[SRG_N];
static int srg_n;
static uint64_t srg_start;
static bool srg_ready;
static struct sx srg_sx;
static int darwin_share_cache = 1;
static int darwin_use_cache = 1;			/* 0: the dyld cache is refused (dyld goes on without it and loads every framework from the disk: a root with our own frameworks) */
SYSCTL_DECL(_kern_darwinator);
SYSCTL_INT(_kern_darwinator, OID_AUTO, use_cache, CTLFLAG_RW, &darwin_use_cache, 0, "0: the shared region of the dyld cache is refused, the frameworks come from the disk");
SYSCTL_INT(_kern_darwinator, OID_AUTO, share_cache, CTLFLAG_RW, &darwin_share_cache, 0, "the processes share the pages of the dyld cache that nobody writes (the first one makes the shared objects)");

/* the object is shared by many mappings now: when the mapping that made it goes away its pages must stay (else the pages of an object that has one mapping are freed with it) */
static void
srg_shared(vm_object_t o)
{
	VM_OBJECT_WLOCK(o);
	vm_object_clear_flag(o, OBJ_ONEMAPPING);
	VM_OBJECT_WUNLOCK(o);
}

/* the bytes of [va, va+size) of this process go into a new anonymous object; the object is returned (one reference), NULL on failure */
static vm_object_t
srg_snapshot(struct thread *td, uint64_t va, uint64_t size)
{
	vm_object_t o;
	char *buf;
	uint64_t done = 0;
	int error = 0;

	o = vm_pager_allocate(OBJT_SWAP, NULL, round_page(size), VM_PROT_DEFAULT, 0, td->td_ucred);
	if (o == NULL)
		return (NULL);
	buf = malloc(16 * PAGE_SIZE, M_DARWIN, M_WAITOK);
	while (done < size && error == 0) {
		size_t n = MIN((uint64_t)16 * PAGE_SIZE, size - done);
		struct iovec iov = { buf, n };
		struct uio uio = { .uio_iov = &iov, .uio_iovcnt = 1, .uio_offset = (off_t)done, .uio_resid = (ssize_t)n, .uio_segflg = UIO_SYSSPACE, .uio_rw = UIO_WRITE, .uio_td = td };

		error = copyin((void *)(va + done), buf, n);
		if (error == 0)
			error = uiomove_object(o, (off_t)round_page(size), &uio);
		done += n;
	}
	free(buf, M_DARWIN);
	if (error != 0) {
		vm_object_deallocate(o);
		return (NULL);
	}
	return (o);
}

static int
srg_mapin(vm_map_t map, struct srg_map *g)
{
	vm_offset_t va = (vm_offset_t)g->va;
	int rv;

	vm_object_reference(g->obj);
	srg_shared(g->obj);
	rv = vm_map_find(map, g->obj, g->off, &va, round_page(g->size), 0, VMFS_NO_SPACE, g->prot, g->maxprot | g->prot, MAP_COPY_ON_WRITE);
	if (rv != KERN_SUCCESS) {
		vm_object_deallocate(g->obj);
		printf("darwinator: %s[%d]: cache sharing: cannot map %#lx +%#lx (%d)\n", curthread->td_proc->p_comm, curthread->td_proc->p_pid, (u_long)g->va, (u_long)g->size, rv);
		return (ENOMEM);
	}
	return (0);
}

/* after the first complete mapping of the cache: make the shared objects and map them in this process in place of the private mappings */
static void
srg_capture(struct thread *td, struct smap *maps, uint32_t nm, uint64_t slide, uint64_t first)
{
	vm_map_t map = &td->td_proc->p_vmspace->vm_map;
	struct srg_map tmp[SRG_N];
	uint32_t i;
	int n = 0;

	if (!darwin_share_cache || nm > SRG_N)
		return;
	sx_xlock(&srg_sx);
	if (srg_ready) {
		sx_xunlock(&srg_sx);
		return;
	}
	for (i = 0; i < nm; i++) {
		struct smap *m = &maps[i];
		uint64_t va = m->addr + slide;
		vm_map_entry_t e;
		struct srg_map *g = &tmp[n];
		bool wr = (m->init_prot & VM_PROT_WRITE) != 0 || (m->max_prot & SMAP_ZF) != 0;

		memset(g, 0, sizeof(*g));
		g->va = va; g->size = m->size; g->prot = (vm_prot_t)(m->init_prot & VM_PROT_ALL); g->maxprot = (vm_prot_t)(m->max_prot & VM_PROT_ALL);
		if (m->size == 0)
			continue;
		if (wr) {
			g->obj = srg_snapshot(td, va, m->size);
			if (g->obj == NULL)
				goto fail;
			g->off = 0;
			srg_shared(g->obj);
		} else {
			vm_map_lock_read(map);
			if (!vm_map_lookup_entry(map, (vm_offset_t)va, &e) || e->start != va || e->end < va + round_page(m->size) || (e->eflags & MAP_ENTRY_IS_SUB_MAP) != 0 || e->object.vm_object == NULL) {
				vm_map_unlock_read(map);
				goto fail;
			}
			g->obj = e->object.vm_object;
			vm_object_reference(g->obj);
			srg_shared(g->obj);
			g->off = e->offset;
			vm_map_unlock_read(map);
		}
		if (darwin_debug >= 2)
			printf("darwinator: cache sharing: mapping %#lx +%#lx prot %d maxprot %#x %s object type %d\n", (u_long)va, (u_long)m->size, g->prot, m->max_prot, wr ? "copy" : "object", g->obj->type);
		n++;
	}
	/* the private mappings of this process of the writable ranges are replaced by the shared object (copy on write) */
	for (i = 0; i < (uint32_t)n; i++) {
		struct srg_map *g = &tmp[i];

		if (g->off == 0 && g->obj->type == OBJT_SWAP && (g->prot & VM_PROT_WRITE) != 0) {
			vm_map_remove(map, (vm_offset_t)g->va, (vm_offset_t)(g->va + round_page(g->size)));
			if (srg_mapin(map, g) != 0)
				printf("darwinator: cache sharing: cannot map the shared object at %#lx back\n", (u_long)g->va);
		}
	}
	memcpy(srg_m, tmp, sizeof(tmp));
	srg_n = n; srg_start = first; srg_ready = true;
	if (darwin_debug)
		printf("darwinator: the cache is shared now: %d mappings from %#lx\n", n, (u_long)first);
	sx_xunlock(&srg_sx);
	return;
fail:
	for (int k = 0; k < n; k++)
		if (tmp[k].obj != NULL)
			vm_object_deallocate(tmp[k].obj);
	sx_xunlock(&srg_sx);
	if (darwin_debug)
		printf("darwinator: cache sharing: not possible (a mapping is not a single entry)\n");
}

/* a later process: the shared mappings in place (what the kernel does for the shared region of a Mac) */
static int
srg_attach(struct thread *td)
{
	vm_map_t map = &td->td_proc->p_vmspace->vm_map;
	int i, error = 0;

	sx_slock(&srg_sx);
	if (!srg_ready) {
		sx_sunlock(&srg_sx);
		return (ENOMEM);
	}
	for (i = 0; i < srg_n && error == 0; i++)
		error = srg_mapin(map, &srg_m[i]);
	if (error != 0)
		for (int k = 0; k < i; k++)
			vm_map_remove(map, (vm_offset_t)srg_m[k].va, (vm_offset_t)(srg_m[k].va + round_page(srg_m[k].size)));
	sx_sunlock(&srg_sx);
	return (error);
}

/* the slot of an address space in sr_tab (sr_mtx held): its own, else an empty one (no address space), else the next one in turn.  A slot whose start is 0 is NOT free: the address space has said "unmapped" (start 0) and asks again,
 * and a process that starts at the same time must not take the slot over (it did: the second attach then found the cache mapped already and failed, and its clean up removed the mapping) */
static int
sr_slot(struct vmspace *vms)
{
	static int turn;
	int i;

	for (i = 0; i < SR_N; i++)
		if (sr_tab[i].vms == vms) return (i);
	for (i = 0; i < SR_N; i++)
		if (sr_tab[i].vms == NULL) return (i);
	i = turn++ % SR_N;
	sr_tab[i].vms = NULL; sr_tab[i].start = 0; sr_tab[i].attached = false;
	return (i);
}

static int
darwin_shared_region_check_np(struct thread *td, struct darwin_args *uap)
{
	struct vmspace *vms = td->td_proc->p_vmspace;
	uint64_t start = 0;
	vm_map_entry_t e;
	int i;

	mtx_lock(&sr_mtx);
	for (i = 0; i < SR_N; i++)
		if (sr_tab[i].vms == vms && sr_tab[i].start != 0) { start = sr_tab[i].start; break; }
	mtx_unlock(&sr_mtx);
	if (!darwin_use_cache)
		return (ENOMEM);
	if (uap->a[0] == 0) {			/* NULL: dyld has unmapped the region */
		if (start != 0) {
			mtx_lock(&sr_mtx);
			for (i = 0; i < SR_N; i++)
				if (sr_tab[i].vms == vms) sr_tab[i].start = 0;
			mtx_unlock(&sr_mtx);
		}
		return (0);
	}
	if (start == 0 && darwin_share_cache && srg_ready) {		/* mapped by us already (dyld said "unmapped" with NULL and asks again): the same start, nothing is mapped twice */
		mtx_lock(&sr_mtx);
		for (i = 0; i < SR_N; i++)
			if (sr_tab[i].vms == vms && sr_tab[i].attached) {
				vm_map_entry_t e0;
				bool there;

				vm_map_lock_read(&vms->vm_map);
				there = vm_map_lookup_entry(&vms->vm_map, srg_start, &e0);		/* (the table is keyed by the address of the vmspace, which a later process can have again) */
				vm_map_unlock_read(&vms->vm_map);
				if (there) sr_tab[i].start = start = srg_start; else { sr_tab[i].attached = false; sr_tab[i].start = 0; }
				break;
			}
		mtx_unlock(&sr_mtx);
	}
	if (start == 0 && darwin_share_cache && srg_ready) {
		int ae = srg_attach(td);
		if (darwin_debug)
			printf("darwinator: %s[%d]: attach to the shared cache: %d\n", td->td_proc->p_comm, td->td_proc->p_pid, ae);
		if (ae != 0) goto noattach;
		/* the cache of the first process: mapped here the way a Mac maps its shared region */
		start = srg_start;
		mtx_lock(&sr_mtx);
		i = sr_slot(vms); sr_tab[i].vms = vms; sr_tab[i].start = start; sr_tab[i].attached = true;
		mtx_unlock(&sr_mtx);
	}
noattach:
	if (start == 0)
		return (ENOMEM);		/* the region is empty: dyld maps the cache */
	vm_map_lock_read(&vms->vm_map);
	i = vm_map_lookup_entry(&vms->vm_map, start, &e);
	vm_map_unlock_read(&vms->vm_map);
	if (!i)
		return (ENOMEM);
	return (copyout(&start, (void *)uap->a[0], sizeof(start)));
}

/* the slide info of version 2 (dyld_cache_slide_info2): the chains of pointers that have to be moved by the slide */
struct slide2 { uint32_t version, page_size, page_starts_offset, page_starts_count, page_extras_offset, page_extras_count; uint64_t delta_mask, value_add; };

static int
rebase_mapping(struct thread *td, uint64_t addr, uint64_t size, uint64_t info_addr, uint64_t info_size, uint64_t slide)
{
	struct slide2 *si;
	uint8_t *info, *page;
	uint16_t *starts, *extras;
	uint64_t delta_mask, value_add, i;
	int shift, error = 0;

	if (info_size < sizeof(*si) || info_size > (4 << 20))
		return (EINVAL);
	info = malloc(info_size, M_DARWIN, M_WAITOK);
	page = malloc(PAGE_SIZE, M_DARWIN, M_WAITOK);
	error = copyin((void *)(info_addr + slide), info, info_size);
	if (error != 0)
		goto out;
	si = (struct slide2 *)info;
	if (si->version != 2 || si->page_size != PAGE_SIZE || si->page_starts_offset + (uint64_t)si->page_starts_count * 2 > info_size ||
	    si->page_extras_offset + (uint64_t)si->page_extras_count * 2 > info_size || (uint64_t)si->page_starts_count * PAGE_SIZE > size) {
		if (darwin_debug)
			printf("darwinator: slide info of version %u is not understood\n", si->version);
		error = ENOTSUP;
		goto out;
	}
	delta_mask = si->delta_mask; value_add = si->value_add;
	shift = ffsll(delta_mask) - 1 - 2;				/* the delta is in units of 4 bytes */
	starts = (uint16_t *)(info + si->page_starts_offset);
	extras = (uint16_t *)(info + si->page_extras_offset);
	for (i = 0; i < si->page_starts_count && error == 0; i++) {
		uint16_t st = starts[i];
		uint64_t pa = addr + i * PAGE_SIZE;
		uint32_t offs[64], n = 0, k;
		bool dirty = false;

		if (st == 0x4000)				/* DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE */
			continue;
		if (st & 0x8000) {				/* ATTR_EXTRA: a list of chain starts in page_extras */
			uint32_t e = st & 0x3fff;

			for (;;) {
				if (e >= si->page_extras_count || n == 64) { error = EINVAL; break; }
				offs[n++] = (extras[e] & 0x3fff) * 4;
				if (extras[e] & 0x8000)
					break;
				e++;
			}
			if (error != 0)
				break;
		} else
			offs[n++] = (uint32_t)st * 4;
		error = copyin((void *)pa, page, PAGE_SIZE);
		if (error != 0)
			break;
		for (k = 0; k < n; k++) {
			uint32_t off = offs[k];

			for (;;) {
				uint64_t raw, delta, value;

				if (off + 8 > PAGE_SIZE) { error = EINVAL; break; }
				memcpy(&raw, page + off, 8);
				delta = (raw & delta_mask) >> shift;
				value = raw & ~delta_mask;
				if (value != 0) {
					value += value_add + slide;
					memcpy(page + off, &value, 8);
					dirty = true;
				}
				if (delta == 0)
					break;
				off += (uint32_t)delta;
			}
			if (error != 0)
				break;
		}
		if (error == 0 && dirty)
			error = copyout(page, (void *)pa, PAGE_SIZE);
	}
out:
	free(info, M_DARWIN);
	free(page, M_DARWIN);
	return (error);
}

static int
darwin_shared_region_map_and_slide_2_np(struct thread *td, struct darwin_args *uap)	/* (files_count, files, mappings_count, mappings) */
{
	uint32_t nf = (uint32_t)uap->a[0], nm = (uint32_t)uap->a[2], i, j, k = 0, total = 0, k0;
	struct sfile *files;
	struct smap *maps;
	uint64_t first = 0, common_slide = 0;
	int error, n;

	if (!darwin_use_cache)
		return (EPERM);
	if (nf == 0 || nf > 32 || nm == 0 || nm > 256)
		return (EINVAL);
	files = malloc(nf * sizeof(*files), M_DARWIN, M_WAITOK);
	maps = malloc(nm * sizeof(*maps), M_DARWIN, M_WAITOK);
	error = copyin((void *)uap->a[1], files, nf * sizeof(*files));
	if (error == 0)
		error = copyin((void *)uap->a[3], maps, nm * sizeof(*maps));
	if (error != 0)
		goto out;
	for (i = 0; i < nf; i++)
		total += files[i].count;
	if (total != nm) { error = EINVAL; goto out; }
	/* one slide for the whole call: the files with data say it, the file of the dynamic data (fd -1) says 0 and is moved with them */
	for (i = 0; i < nf; i++)
		if (files[i].slide > common_slide)
			common_slide = files[i].slide;
	for (i = 0; i < nf && error == 0; i++) {
		uint64_t slide = common_slide;

		k0 = k;
		/* every mapping at its address plus the slide (the cache is moved as a whole) */
		for (j = 0; j < files[i].count && error == 0; j++, k++) {
			struct smap *m = &maps[k];
			struct mmap_req mr = { 0 };
			bool slid = (m->max_prot & SMAP_SLIDE) != 0 || (m->init_prot & SMAP_SLIDE) != 0;
			int prot = 0, fprot;

			if (m->init_prot & VM_PROT_READ) prot |= PROT_READ;
			if (m->init_prot & VM_PROT_WRITE) prot |= PROT_WRITE;
			if (m->init_prot & VM_PROT_EXECUTE) prot |= PROT_EXEC;
			fprot = prot;
			if ((slid && slide != 0) || files[i].fd == -1)
				prot |= PROT_WRITE;			/* written below, then the protection of the mapping is set */
			mr.mr_hint = m->addr + slide;
			mr.mr_len = m->size;
			mr.mr_prot = prot;
			if (files[i].fd == -1 || (m->max_prot & SMAP_ZF)) {
				mr.mr_flags = MAP_FIXED | MAP_ANON | MAP_PRIVATE;
				mr.mr_fd = -1;
			} else {
				mr.mr_flags = MAP_FIXED | MAP_PRIVATE;
				mr.mr_fd = files[i].fd;
				mr.mr_pos = m->off;
			}
			error = kern_mmap(td, &mr);
			if (error != 0) {
				if (darwin_debug)
					printf("darwinator: shared region map %#lx failed: %d\n", (u_long)(m->addr + slide), error);
				break;
			}
			if (files[i].fd == -1 && m->off != 0) {		/* the dynamic data: the bytes are in the memory of dyld (the "offset" is their address) */
				char *tmp = malloc(m->size, M_DARWIN, M_WAITOK);

				error = copyin((void *)m->off, tmp, m->size);
				if (error == 0)
					error = copyout(tmp, (void *)(m->addr + slide), m->size);
				free(tmp, M_DARWIN);
			}
			if (error == 0 && files[i].fd == -1 && fprot != prot)
				error = kern_mprotect(td, m->addr + slide, m->size, fprot, 0);
			if (error == 0 && first == 0)
				first = m->addr + slide;
		}
		/* the pointers of the data mappings are moved by the slide */
		for (j = 0, k = k0; j < files[i].count && error == 0; j++, k++) {
			struct smap *m = &maps[k];
			bool slid = (m->max_prot & SMAP_SLIDE) != 0 || (m->init_prot & SMAP_SLIDE) != 0;

			if (slid && slide != 0 && m->slide_size != 0 && files[i].fd != -1) {
				int prot = 0;

				error = rebase_mapping(td, m->addr + slide, m->size, m->slide_start, m->slide_size, slide);
				if (m->init_prot & VM_PROT_READ) prot |= PROT_READ;
				if (m->init_prot & VM_PROT_WRITE) prot |= PROT_WRITE;
				if (m->init_prot & VM_PROT_EXECUTE) prot |= PROT_EXEC;
				if (error == 0 && !(prot & PROT_WRITE))
					error = kern_mprotect(td, m->addr + slide, m->size, prot, 0);
			}
		}
	}
	if (error == 0 && first != 0) {
		struct vmspace *vms = td->td_proc->p_vmspace;

		mtx_lock(&sr_mtx);
		n = sr_slot(vms);
		sr_tab[n].vms = vms; sr_tab[n].start = first; sr_tab[n].attached = true;		/* (its own mapping or the shared one: either way the cache is there) */
		mtx_unlock(&sr_mtx);
		srg_capture(td, maps, nm, common_slide, first);
	}
out:
	if (darwin_debug)
		printf("darwinator: shared_region_map_and_slide_2_np: %d, first mapping %#lx\n", error, (u_long)first);
	if (darwin_debug >= 2) {
		vm_map_t map = &td->td_proc->p_vmspace->vm_map;
		vm_map_entry_t e;

		vm_map_lock_read(map);
		VM_MAP_ENTRY_FOREACH(e, map)
			if (e->start >= 0x7ffe90000000UL && e->start < 0x7ffeb0000000UL)
				printf("darwinator:   map entry %#lx - %#lx prot %x\n", (u_long)e->start, (u_long)e->end, e->protection);
		vm_map_unlock_read(map);
	}
	free(files, M_DARWIN);
	free(maps, M_DARWIN);
	return (error);
}

/* the module goes: the objects of the shared cache (swap backed, 1.7 GB with the pages of the data that were slid) are given back (each reload of the module leaked them and the swap ran full) */
void
darwin_dyld_syscalls_fini(void)
{
	int i;

	if (!sr_inited)
		return;
	sx_xlock(&srg_sx);
	if (srg_ready) {
		for (i = 0; i < srg_n; i++)
			if (srg_m[i].obj != NULL)
				vm_object_deallocate(srg_m[i].obj);
		srg_n = 0; srg_ready = false;
	}
	sx_xunlock(&srg_sx);
}

void
darwin_dyld_syscalls_init(void)
{
	if (!sr_inited) {
		mtx_init(&sr_mtx, "darwin shared region", NULL, MTX_DEF);
		sx_init(&srg_sx, "darwin shared cache");
		sr_inited = true;
	}
	darwin_sysent[294] = (struct sysent){ .sy_narg = 1, .sy_call = (sy_call_t *)darwin_shared_region_check_np, .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
	darwin_syscallnames[294] = "shared_region_check_np";
	darwin_sysent[536] = (struct sysent){ .sy_narg = 4, .sy_call = (sy_call_t *)darwin_shared_region_map_and_slide_2_np, .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
	darwin_syscallnames[536] = "shared_region_map_and_slide_2_np";
}
