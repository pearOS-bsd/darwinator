/* IOSurface: the user client of IOSurfaceRoot.
 * What the IOSurface framework of the Mac does (found with SwiftUI/tools/msgtrace.c on a Mac and in the code of IOSurface.framework; SwiftUI/tests/iosurface, SwiftUI/docs/EVIDENCE.md):
 *   IOServiceOpen(IOSurfaceRoot), then
 *   selector 13: no input, 40 bytes out (the limits of a surface);
 *   selector 44, one scalar: "not found" (kIOReturnNotFound) here;
 *   selector 0 (create), one scalar (the id of the parent surface, 0), the properties as a binary serialized dictionary, 0xc68 bytes out: the surface as the kernel made it, among them the address
 *   (u64 @0) at which the memory of the surface is mapped in the calling task, two pointers into a region that the kernel keeps for the client (@8, @16), the id (@24), the allocation size (@32), width, height,
 *   bytes per row (@40, @48, @56), the pixel format (@72), the allocation size again (@80), bytes per element and two flag bytes (@96), a counter (@120);
 *   the fast methods are mach trap 100 (iokit_user_client_trap): 2 lock, 3 unlock, 5 release.
 * The memory of a surface is an object of the VM that every client of the surface can map: a POSIX shared memory object (shm_alloc), so that a native program of the system (the Metal helper,
 * the window server) can have it as a file descriptor through /dev/iosurface (iosurface_dev.h); the Mac's IOSurface is a memory descriptor that the GPU driver and WindowServer map the same way. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sysent.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include "darwinator.h"
#include "iosurface.h"
#include "iosurface_dev.h"

#define IOS_MAX		256
#define IOS_KERN_SUCCESS	0		/* kern_return_t of Mach */
#define IOS_KERN_INVALID_ADDRESS 1
#define IOS_NOTBADARG	((int)0xe00002c2)	/* kIOReturnBadArgument */
#define IOS_NORESOURCES	((int)0xe00002bd)	/* kIOReturnNoResources */
#define IOS_UNSUPPORTED	((int)0xe00002c7)	/* kIOReturnUnsupported */

#define IOS_USERS	16
/* a surface and its users: as in IOSurfaceRootUserClient on the Mac, each process that made or looked up a surface has one client of it with a use count (create and lookup add one, the release trap takes
 * one away); when the count of a process goes to 0 the memory is unmapped from it, and when no process uses the surface any more the surface goes.  A process that exits loses its uses. */
struct ios_user { pid_t pid; uint32_t uses; vm_offset_t va, iva; };
static struct iosurf { bool used, global; uint32_t id, w, h, row, fmt, bpe; uint64_t alloc; uid_t uid; struct shmfd *shm; struct vm_object *obj; struct ios_user u[IOS_USERS]; } iosurfs[IOS_MAX];
/* the table owns one reference to the shared memory of each surface (shm); the shm owns the VM object; every mapping and every descriptor of /dev/iosurface holds its own reference */
static uint32_t iosurf_next = 1, iosurf_counter = 0x494;
static struct mtx ios_mtx;
MTX_SYSINIT(darwin_iosurf, &ios_mtx, "darwin iosurface", MTX_DEF);

static uint32_t
le32(const uint8_t *b, size_t off)
{
	uint32_t v;

	memcpy(&v, b + off, sizeof(v));
	return (v);
}

/* OSSerializeBinary: the signature 0xd3, then one object: a header word (the type in bits 24-30, 0x80000000 on the last item of a collection, the length or the count in bits 0-23) and its bytes.
 * Types: 0x01 dictionary (count pairs: a key and a value, the key is a symbol), 0x02 array and 0x03 set (count objects), 0x04 number (the header holds the bits, 8 bytes follow), 0x08 symbol,
 * 0x09 string, 0x0a data (the length, the bytes padded to 4), 0x0b boolean (the value is the low bit), 0x0c a reference to an object seen before (no bytes).  CoreImage puts collections among the
 * properties of a surface (the planes ...): they are skipped as a whole, the keys after them are read. */
static int
ios_skip(const uint8_t *b, size_t n, size_t *pp, int depth)	/* steps over one object; 0 when the bytes are not a valid object */
{
	size_t p = *pp;
	uint32_t h, type, len, i;

	if (depth > 16 || p + 4 > n)
		return (0);
	h = le32(b, p); p += 4;
	type = h & 0x7f000000u; len = h & 0x00ffffff;
	switch (type) {
	case 0x01000000u:
		for (i = 0; i < 2 * len; i++)
			if (!ios_skip(b, n, &p, depth + 1))
				return (0);
		break;
	case 0x02000000u:
	case 0x03000000u:
		for (i = 0; i < len; i++)
			if (!ios_skip(b, n, &p, depth + 1))
				return (0);
		break;
	case 0x04000000u:
		p += 8;
		break;
	case 0x08000000u:
	case 0x09000000u:
	case 0x0a000000u:
		p += roundup2(len, 4);
		break;
	case 0x0b000000u:
	case 0x0c000000u:
		break;
	default:
		return (0);
	}
	if (p > n)
		return (0);
	*pp = p;
	return (1);
}

/* the object of an index: the objects are numbered in the order of the stream (the top dictionary is 0, keys count, references do not), as OSUnserializeBinary keeps them */
static int
ios_object_at(const uint8_t *b, size_t n, uint32_t want, uint32_t *vh, size_t *vp)
{
	size_t p = 4;
	uint32_t idx = 0, h, type, len;

	while (p + 4 <= n) {
		h = le32(b, p);
		type = h & 0x7f000000u; len = h & 0x00ffffff;
		if (type != 0x0c000000u) {
			if (idx == want) {
				*vh = h;
				*vp = p + 4;
				return (1);
			}
			idx++;
		}
		p += 4;
		if (type == 0x04000000u)
			p += 8;
		else if (type == 0x08000000u || type == 0x09000000u || type == 0x0a000000u)
			p += roundup2(len, 4);
		else if (type < 0x01000000u || type > 0x0c000000u)
			return (0);
	}
	return (0);
}

/* the header word and the offset of the bytes of the value of a key of the top dictionary; 0 when the key is not there */
static int
ios_dict_find(const uint8_t *b, size_t n, const char *want, uint32_t *vh, size_t *vp)
{
	size_t p = 4, wl = strlen(want) + 1;
	uint32_t hdr, cnt, i;

	if (n < 8 || le32(b, 0) != 0xd3)
		return (0);
	hdr = le32(b, p); p += 4;
	if ((hdr & 0x7f000000u) != 0x01000000u)
		return (0);
	cnt = hdr & 0x00ffffff;
	for (i = 0; i < cnt && p + 4 <= n; i++) {
		uint32_t kh = le32(b, p), klen = kh & 0x00ffffff;
		bool match;

		if ((kh & 0x7f000000u) != 0x08000000u || p + 4 + roundup2(klen, 4) + 4 > n)
			return (0);
		match = klen == wl && strncmp((const char *)b + p + 4, want, klen) == 0;
		p += 4 + roundup2(klen, 4);
		if (match) {
			*vh = le32(b, p);
			*vp = p + 4;
			if ((*vh & 0x7f000000u) == 0x0c000000u)		/* the same object as one before (IOSurfaceHeight is the CFNumber of IOSurfaceWidth when they are equal) */
				return (ios_object_at(b, n, *vh & 0x00ffffff, vh, vp));
			return (1);
		}
		if (!ios_skip(b, n, &p, 1))
			return (0);
	}
	return (0);
}

/* debugging: the keys of the top dictionary and the header word of each value */
static void
ios_dict_dump(const uint8_t *b, size_t n)
{
	size_t p = 8;
	uint32_t cnt, i;

	if (n < 8 || le32(b, 0) != 0xd3)
		return;
	cnt = le32(b, 4) & 0x00ffffff;
	for (i = 0; i < cnt && p + 4 <= n; i++) {
		uint32_t kh = le32(b, p), klen = kh & 0x00ffffff;
		char key[64];

		if (p + 4 + roundup2(klen, 4) + 4 > n)
			return;
		strlcpy(key, (const char *)b + p + 4, MIN(sizeof(key), klen + 1));
		p += 4 + roundup2(klen, 4);
		printf("darwinator:   key %#x %s: value %#x%s\n", kh, (kh & 0x7f000000u) == 0x08000000u ? key : "(not a symbol)", le32(b, p),
		    (le32(b, p) & 0x7f000000u) == 0x04000000u && p + 12 <= n ? "" : "");
		if ((kh & 0x7f000000u) != 0x08000000u || !ios_skip(b, n, &p, 1))
			return;
	}
}

int
iosurface_dict_number(const uint8_t *b, size_t n, const char *want, uint64_t *out)
{
	uint32_t vh;
	size_t vp;

	if (!ios_dict_find(b, n, want, &vh, &vp) || (vh & 0x7f000000u) != 0x04000000u || vp + 8 > n)
		return (0);
	memcpy(out, b + vp, 8);
	return (1);
}

/* a boolean value (type 0x0b, the value is the low bit of the header) of a key: 1 when the key is there and true */
static int
ios_dict_true(const uint8_t *b, size_t n, const char *want)
{
	uint32_t vh;
	size_t vp;

	return (ios_dict_find(b, n, want, &vh, &vp) && (vh & 0x7f000000u) == 0x0b000000u && (vh & 1) != 0);
}

static int
ios_map(struct proc *tp, struct vm_object *obj, uint64_t size, vm_offset_t *va)
{
	struct vm_map *map = &tp->p_vmspace->vm_map;
	int rv;

	*va = 0x200000000ULL;
	vm_object_reference(obj);
	rv = vm_map_find(map, obj, 0, va, round_page(size), 0, VMFS_ANY_SPACE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE, MAP_INHERIT_SHARE);
	if (rv != IOS_KERN_SUCCESS)
		vm_object_deallocate(obj);
	return (rv);
}

void
iosurface_limits(uint8_t *out)
{
	static const uint8_t o13[IOSURF_LIMITS_SIZE] = { 0x3f, 0, 0, 0, 0x3f, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 };

	memcpy(out, o13, sizeof(o13));
}

/* the structure of 0xc68 bytes for a surface that is mapped at va, with the region that the kernel keeps for the client at iva */
static void
ios_fill_addrs(struct iosurf *sf, vm_offset_t va, vm_offset_t iva, uint8_t *out)
{
	uint64_t v, w = sf->w, h = sf->h, row = sf->row, fmt = sf->fmt, bpe = sf->bpe, alloc = sf->alloc;

	if (iva == 0)
		iva = va;
	v = va; memcpy(out + 0, &v, 8);
	v = iva + 0x4e0; memcpy(out + 8, &v, 8);
	v = iva + 0x3180; memcpy(out + 16, &v, 8);
	v = sf->id; memcpy(out + 24, &v, 8);
	memcpy(out + 32, &alloc, 8);
	memcpy(out + 40, &w, 8);
	memcpy(out + 48, &h, 8);
	memcpy(out + 56, &row, 8);
	memcpy(out + 72, &fmt, 8);
	memcpy(out + 80, &alloc, 8);
	out[96] = (uint8_t)bpe; out[97] = 0; out[98] = 1; out[99] = 1;
	v = 0x0100000000000000ULL | iosurf_counter++; memcpy(out + 120, &v, 8);	/* the tail of the structure (the planes) stays zero: a surface of one plane */
}

/* the same for a surface that was just mapped in the task of td at va: the client region is made and mapped too (its address in *ivap, 0 when it could not be mapped) */
static int
ios_fill(struct thread *td, struct iosurf *sf, vm_offset_t va, uint8_t *out, vm_offset_t *ivap)
{
	vm_offset_t iva = 0;
	struct vm_object *io;
	int rv;

	io = vm_object_allocate(OBJT_DEFAULT, 16);			/* the region that the kernel keeps for the client (the state of the surface: locks, seeds): zero pages */
	rv = ios_map(td->td_proc, io, 16 * PAGE_SIZE, &iva);
	vm_object_deallocate(io);
	*ivap = rv == IOS_KERN_SUCCESS ? iva : 0;
	ios_fill_addrs(sf, va, *ivap, out);
	return (0);
}

/* the user entry of process p in sf (made when add is true); NULL when there is none (or no room).  ios_mtx held */
static struct ios_user *
ios_user_of(struct iosurf *sf, pid_t pid, bool add)
{
	int i;

	for (i = 0; i < IOS_USERS; i++)
		if (sf->u[i].uses != 0 && sf->u[i].pid == pid)
			return (&sf->u[i]);
	if (add)
		for (i = 0; i < IOS_USERS; i++)
			if (sf->u[i].uses == 0) { memset(&sf->u[i], 0, sizeof(sf->u[i])); sf->u[i].pid = pid; return (&sf->u[i]); }
	return (NULL);
}

/* no process uses sf any more: the slot is free and the table's reference to the memory goes (the pages go with the last mapping).  ios_mtx held; the object to deallocate (outside the lock) is returned */
static struct shmfd *
ios_drop_if_unused(struct iosurf *sf)
{
	struct shmfd *o;
	int i;

	for (i = 0; i < IOS_USERS; i++)
		if (sf->u[i].uses != 0)
			return (NULL);
	o = sf->shm;
	if (darwin_debug >= 1)
		printf("darwinator: IOSurface %u freed (%ju bytes)\n", sf->id, (uintmax_t)sf->alloc);
	memset(sf, 0, sizeof(*sf));
	return (o);
}

/* a process that exits: its uses of every surface go (its address space goes with it, so nothing is unmapped here) */
void
iosurface_proc_exit(struct proc *p)
{
	struct shmfd *drop[IOS_MAX];
	int i, j, n = 0;

	mtx_lock(&ios_mtx);
	for (i = 0; i < IOS_MAX; i++) {
		struct ios_user *u;

		if (!iosurfs[i].used || (u = ios_user_of(&iosurfs[i], p->p_pid, false)) == NULL)
			continue;
		u->uses = 0;
		if ((drop[n] = ios_drop_if_unused(&iosurfs[i])) != NULL)
			n++;
	}
	mtx_unlock(&ios_mtx);
	for (j = 0; j < n; j++)
		shm_drop(drop[j]);
}

int
iosurface_create(struct thread *td, const uint8_t *props, size_t len, uint8_t *out)
{
	uint64_t w = 0, h = 0, bpe = 4, fmt = 0, row = 0, alloc = 0;
	struct iosurf *sf = NULL;
	vm_offset_t va = 0, iva = 0;
	int i;

	(void)iosurface_dict_number(props, len, "IOSurfaceWidth", &w);
	(void)iosurface_dict_number(props, len, "IOSurfaceHeight", &h);
	(void)iosurface_dict_number(props, len, "IOSurfaceBytesPerElement", &bpe);
	(void)iosurface_dict_number(props, len, "IOSurfacePixelFormat", &fmt);
	(void)iosurface_dict_number(props, len, "IOSurfaceBytesPerRow", &row);
	(void)iosurface_dict_number(props, len, "IOSurfaceAllocSize", &alloc);
	if (w == 0 || h == 0 || w > 16384 || h > 16384 || bpe == 0 || bpe > 16) {
		if (darwin_debug >= 1)
		{
			printf("darwinator: IOSurface not made: width %ju height %ju bytes per element %ju (%zu bytes of properties)\n", (uintmax_t)w, (uintmax_t)h, (uintmax_t)bpe, len);
			ios_dict_dump(props, len);
		}
		return (IOS_NOTBADARG);
	}
	if (row == 0)
		row = roundup2(w * bpe, 64);
	if (alloc == 0)
		alloc = row * h;
	mtx_lock(&ios_mtx);
	for (i = 0; i < IOS_MAX; i++)
		if (!iosurfs[i].used) { sf = &iosurfs[i]; break; }
	if (sf != NULL) { memset(sf, 0, sizeof(*sf)); sf->used = true; sf->id = iosurf_next++; }
	mtx_unlock(&ios_mtx);
	if (sf == NULL)
		return (IOS_NORESOURCES);
	sf->w = (uint32_t)w; sf->h = (uint32_t)h; sf->row = (uint32_t)row; sf->fmt = (uint32_t)fmt; sf->bpe = (uint32_t)bpe; sf->alloc = alloc; sf->global = ios_dict_true(props, len, "IOSurfaceIsGlobal");
	sf->uid = td->td_ucred->cr_uid;
	sf->shm = shm_alloc(td->td_ucred, 0600, false);
	if (sf->shm == NULL || shm_dotruncate(sf->shm, round_page(alloc)) != 0) {
		if (sf->shm != NULL)
			shm_drop(sf->shm);
		sf->shm = NULL;
		sf->used = false;
		return (IOS_NORESOURCES);
	}
	sf->obj = sf->shm->shm_object;
	if (ios_map(td->td_proc, sf->obj, alloc, &va) != IOS_KERN_SUCCESS) {
		shm_drop(sf->shm);
		sf->shm = NULL;
		sf->used = false;
		return (IOS_NORESOURCES);
	}
	if (ios_fill(td, sf, va, out, &iva) != 0)
		return (IOS_NORESOURCES);
	mtx_lock(&ios_mtx);
	sf->u[0].pid = td->td_proc->p_pid; sf->u[0].uses = 1; sf->u[0].va = va; sf->u[0].iva = iva;
	mtx_unlock(&ios_mtx);
	if (darwin_debug >= 1)
		printf("darwinator: IOSurface %u made: %ux%u, row %u, %ju bytes, format %#x\n", sf->id, sf->w, sf->h, sf->row, (uintmax_t)alloc, sf->fmt);
	return (0);
}

static int ios_lookup(struct thread *td, uint64_t id, uint8_t *out, bool by_port);

int
iosurface_lookup(struct thread *td, uint64_t id, uint8_t *out)
{
	return (ios_lookup(td, id, out, false));
}

/* IOSurfaceLookupFromMachPort (selector 34): the surface of a port that IOSurfaceCreateMachPort made; a port gives access whether the surface is global or not */
int
iosurface_lookup_port(struct thread *td, uint32_t id, uint8_t *out)
{
	return (ios_lookup(td, id, out, true));
}

/* the ports of a surface (IOSurfaceCreateMachPort, selector 35): each one is a use of the surface by "the ports" (pid -1), so the surface stays while a port names it (in a message on its way, in a
 * task that has not looked it up yet); the port's death takes the use away */
int
iosurface_port_ref(struct thread *td, uint32_t id)
{
	struct ios_user *u;
	int i, rv = IOS_NOTBADARG;

	mtx_lock(&ios_mtx);
	for (i = 0; i < IOS_MAX; i++)
		if (iosurfs[i].used && iosurfs[i].id == id) {
			/* the caller must have the surface (a client of it on the Mac) */
			if (ios_user_of(&iosurfs[i], td->td_proc->p_pid, false) != NULL && (u = ios_user_of(&iosurfs[i], -1, true)) != NULL) { u->uses++; rv = 0; }
			break;
		}
	mtx_unlock(&ios_mtx);
	return (rv);
}

void
iosurface_port_unref(uint32_t id)
{
	struct shmfd *o = NULL;
	struct ios_user *u;
	int i;

	mtx_lock(&ios_mtx);
	for (i = 0; i < IOS_MAX; i++)
		if (iosurfs[i].used && iosurfs[i].id == id) {
			if ((u = ios_user_of(&iosurfs[i], -1, false)) != NULL && --u->uses == 0)
				o = ios_drop_if_unused(&iosurfs[i]);
			break;
		}
	mtx_unlock(&ios_mtx);
	if (o != NULL)
		shm_drop(o);
}

/* lock (id, options, *seed), unlock (id, options, *seed), release (id): a surface is not locked against anybody here, the seed stays 0 */
int
iosurface_trap(struct thread *td, uint32_t index, uint64_t id, uint64_t options, uint64_t seed_user_addr)
{
	uint32_t seed = 0;

	(void)options;
	if (index == IOSURF_TRAP_LOCK || index == IOSURF_TRAP_UNLOCK) {
		if (seed_user_addr != 0 && copyout(&seed, (void *)seed_user_addr, sizeof(seed)) != 0)
			return (IOS_KERN_INVALID_ADDRESS);
		return (0);
	}
	if (index == IOSURF_TRAP_RELEASE) {
		struct vm_map *map = &td->td_proc->p_vmspace->vm_map;
		struct shmfd *o = NULL;
		vm_offset_t va = 0, iva = 0;
		uint64_t size = 0;
		int i, rv = IOS_NOTBADARG;

		mtx_lock(&ios_mtx);
		for (i = 0; i < IOS_MAX; i++) {
			struct ios_user *u;

			if (!iosurfs[i].used || iosurfs[i].id != id)
				continue;
			if ((u = ios_user_of(&iosurfs[i], td->td_proc->p_pid, false)) != NULL) {
				rv = 0;
				if (--u->uses == 0) {		/* the client of this process goes: its mappings go (IOSurfaceClient on the Mac releases its memory map) */
					va = u->va; iva = u->iva; size = iosurfs[i].alloc;
					o = ios_drop_if_unused(&iosurfs[i]);
				}
			}
			break;
		}
		mtx_unlock(&ios_mtx);
		if (va != 0)
			vm_map_remove(map, va, va + round_page(size));
		if (iva != 0)
			vm_map_remove(map, iva, iva + 16 * PAGE_SIZE);
		if (o != NULL)
			shm_drop(o);
		return (rv);
	}
	return (IOS_UNSUPPORTED);
}

/* selector 4: the surface of an id (the scalar) that another process made and marked global; the memory is mapped into the caller and the structure is the one of create.  kIOReturnBadArgument for an id
 * that is not there or not global: what the Mac answers */
static int
ios_lookup(struct thread *td, uint64_t id, uint8_t *out, bool by_port)
{
	struct iosurf *sf = NULL;
	struct ios_user *u;
	vm_offset_t va = 0, iva = 0;
	int i;

	mtx_lock(&ios_mtx);
	for (i = 0; i < IOS_MAX; i++)
		if (iosurfs[i].used && iosurfs[i].id == id && (iosurfs[i].global || by_port)) { sf = &iosurfs[i]; break; }
	if (sf != NULL && (u = ios_user_of(sf, td->td_proc->p_pid, false)) != NULL && u->va != 0) {
		if (by_port) {			/* the Mac's answer to a look-up from a port by a process that has the surface already: -1 and the id; the framework takes its cached client (IOSurfaceClientLookupFromMachPort, decompiled) */
			mtx_unlock(&ios_mtx);
			memset(out, 0xff, 8);
			memcpy(out + 24, &sf->id, 4);
			return (0);
		}
		u->uses++;			/* a process that has the surface already: one more use of the same mapping */
		va = u->va; iva = u->iva;
	}
	if (sf != NULL)
		vm_object_reference(sf->obj);	/* the surface stays while it is mapped here (another process may release it meanwhile) */
	mtx_unlock(&ios_mtx);
	if (sf == NULL)
		return (IOS_NOTBADARG);
	if (va != 0) {
		vm_object_deallocate(sf->obj);
		/* the same structure as before, with the addresses of the existing mapping */
		ios_fill_addrs(sf, va, iva, out);
		return (0);
	}
	if (ios_map(td->td_proc, sf->obj, sf->alloc, &va) != IOS_KERN_SUCCESS) {
		vm_object_deallocate(sf->obj);
		return (IOS_NORESOURCES);
	}
	vm_object_deallocate(sf->obj);
	if (ios_fill(td, sf, va, out, &iva) != 0)
		return (IOS_NORESOURCES);
	mtx_lock(&ios_mtx);
	if (sf->used && sf->id == id && (u = ios_user_of(sf, td->td_proc->p_pid, true)) != NULL) { u->uses++; u->va = va; u->iva = iva; }
	mtx_unlock(&ios_mtx);
	return (0);
}

/* ---- /dev/iosurface: the memory of a surface as a file descriptor, for the native programs of the system (iosurface_dev.h) ---- */

static int
iosurface_dev_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
	struct iosurf_open *io = (struct iosurf_open *)data;
	struct iosurf *sf = NULL;
	struct shmfd *shm = NULL;
	struct file *fp;
	int i, fd, error;

	(void)dev; (void)fflag;
	if (cmd != IOSURF_IOC_OPEN)
		return (ENOTTY);
	mtx_lock(&ios_mtx);
	for (i = 0; i < IOS_MAX; i++)
		if (iosurfs[i].used && iosurfs[i].id == io->id && iosurfs[i].shm != NULL) { sf = &iosurfs[i]; break; }
	/* a global surface, one of the caller's user, or any for the superuser (the window server and the Metal helper run as root) */
	if (sf != NULL && (sf->global || sf->uid == td->td_ucred->cr_uid || td->td_ucred->cr_uid == 0)) {
		shm = shm_hold(sf->shm);
		io->width = sf->w; io->height = sf->h; io->bytes_per_row = sf->row; io->pixel_format = sf->fmt; io->bytes_per_element = sf->bpe; io->alloc_size = sf->alloc;
	}
	mtx_unlock(&ios_mtx);
	if (sf == NULL)
		return (ENOENT);
	if (shm == NULL)
		return (EPERM);
	error = falloc(td, &fp, &fd, O_CLOEXEC);
	if (error != 0) {
		shm_drop(shm);
		return (error);
	}
	finit(fp, FREAD | FWRITE, DTYPE_SHM, shm, &shm_ops);	/* the reference of shm_hold goes to the file */
	fdrop(fp, td);
	io->fd = fd;
	return (0);
}

static struct cdevsw iosurface_cdevsw = {
	.d_version = D_VERSION,
	.d_name = "iosurface",
	.d_ioctl = iosurface_dev_ioctl,
};
static struct cdev *iosurface_cdev;

void
iosurface_dev_init(void)
{
	iosurface_cdev = make_dev(&iosurface_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "iosurface");
}

void
iosurface_dev_fini(void)
{
	if (iosurface_cdev != NULL)
		destroy_dev(iosurface_cdev);
	iosurface_cdev = NULL;
}
