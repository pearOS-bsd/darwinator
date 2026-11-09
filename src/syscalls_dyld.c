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
#include <sys/sysproto.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

struct sfile { int32_t fd; uint32_t count; uint32_t slide; };		/* shared_file_np */
struct smap { uint64_t addr, size, off, slide_size, slide_start; uint32_t max_prot, init_prot; };	/* shared_file_mapping_slide_np */
_Static_assert(sizeof(struct sfile) == 12, "shared_file_np");
_Static_assert(sizeof(struct smap) == 48, "shared_file_mapping_slide_np");

#define	SMAP_COW	0x08
#define	SMAP_ZF		0x10
#define	SMAP_SLIDE	0x20

#define	SR_N	64
static struct { struct vmspace *vms; uint64_t start; } sr_tab[SR_N];
static struct mtx sr_mtx;
static bool sr_inited;

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
	if (uap->a[0] == 0) {			/* NULL: dyld has unmapped the region */
		if (start != 0) {
			mtx_lock(&sr_mtx);
			for (i = 0; i < SR_N; i++)
				if (sr_tab[i].vms == vms) sr_tab[i].start = 0;
			mtx_unlock(&sr_mtx);
		}
		return (0);
	}
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
		n = -1;
		for (i = 0; i < SR_N; i++)
			if (sr_tab[i].vms == vms || sr_tab[i].start == 0) { n = i; break; }
		if (n < 0)
			n = (int)(((uintptr_t)vms >> 8) % SR_N);
		sr_tab[n].vms = vms; sr_tab[n].start = first;
		mtx_unlock(&sr_mtx);
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

void
darwin_dyld_syscalls_init(void)
{
	if (!sr_inited) {
		mtx_init(&sr_mtx, "darwin shared region", NULL, MTX_DEF);
		sr_inited = true;
	}
	darwin_sysent[294] = (struct sysent){ .sy_narg = 1, .sy_call = (sy_call_t *)darwin_shared_region_check_np, .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
	darwin_syscallnames[294] = "shared_region_check_np";
	darwin_sysent[536] = (struct sysent){ .sy_narg = 4, .sy_call = (sy_call_t *)darwin_shared_region_map_and_slide_2_np, .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
	darwin_syscallnames[536] = "shared_region_map_and_slide_2_np";
}
