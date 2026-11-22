/* IOSurface: the user client of IOSurfaceRoot.
 * What the IOSurface framework of the Mac does (found with SwiftUI/tools/msgtrace.c on a Mac and in the code of IOSurface.framework; SwiftUI/tests/iosurface, SwiftUI/docs/EVIDENCE.md):
 *   IOServiceOpen(IOSurfaceRoot), then
 *   selector 13: no input, 40 bytes out (the limits of a surface);
 *   selector 44, one scalar: "not found" (kIOReturnNotFound) here;
 *   selector 0 (create), one scalar (the id of the parent surface, 0), the properties as a binary serialized dictionary, 0xc68 bytes out: the surface as the kernel made it, among them the address
 *   (u64 @0) at which the memory of the surface is mapped in the calling task, two pointers into a region that the kernel keeps for the client (@8, @16), the id (@24), the allocation size (@32), width, height,
 *   bytes per row (@40, @48, @56), the pixel format (@72), the allocation size again (@80), bytes per element and two flag bytes (@96), a counter (@120);
 *   the fast methods are mach trap 100 (iokit_user_client_trap): 2 lock, 3 unlock, 5 release.
 * The memory of a surface is an object of the VM that every client of the surface can map. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sysent.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include "darwinator.h"
#include "iosurface.h"

#define IOS_MAX		256
#define IOS_KERN_SUCCESS	0		/* kern_return_t of Mach */
#define IOS_KERN_INVALID_ADDRESS 1
#define IOS_NOTBADARG	((int)0xe00002c2)	/* kIOReturnBadArgument */
#define IOS_NORESOURCES	((int)0xe00002bd)	/* kIOReturnNoResources */
#define IOS_UNSUPPORTED	((int)0xe00002c7)	/* kIOReturnUnsupported */

static struct iosurf { bool used, global; uint32_t id, w, h, row, fmt, bpe; uint64_t alloc; struct vm_object *obj; } iosurfs[IOS_MAX];
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

/* OSSerializeBinary: the signature 0xd3, the header of the dictionary (0x01 and the number of pairs, 0x80 for the last item of a collection), then key / value pairs: a symbol (0x08, the length with the
 * terminating 0, the text padded to 4), the value: a number (0x04: the header holds the bits, 8 bytes follow), a string (0x09), data (0x0a) with the length, a boolean (0x0b) or an object (0x0c) without bytes */
int
iosurface_dict_number(const uint8_t *b, size_t n, const char *want, uint64_t *out)
{
	size_t p = 4;
	uint32_t hdr, cnt, i;

	if (n < 8 || le32(b, 0) != 0xd3)
		return (0);
	hdr = le32(b, p); p += 4;
	if ((hdr & 0x7f000000u) != 0x01000000u)
		return (0);
	cnt = hdr & 0x00ffffff;
	for (i = 0; i < cnt && p + 4 <= n; i++) {
		uint32_t kh = le32(b, p), klen = kh & 0x00ffffff, vh, vlen, type;
		bool match;

		p += 4;
		if ((kh & 0x7f000000u) != 0x08000000u || p + roundup2(klen, 4) + 4 > n)
			return (0);
		match = klen > 0 && strlen(want) + 1 == klen && strncmp((const char *)b + p, want, klen) == 0;
		p += roundup2(klen, 4);
		vh = le32(b, p); p += 4;
		type = vh & 0x7f000000u; vlen = vh & 0x00ffffff;
		if (type == 0x04000000u) {
			if (p + 8 > n)
				return (0);
			if (match) {
				memcpy(out, b + p, 8);
				return (1);
			}
			p += 8;
		} else if (type == 0x09000000u || type == 0x08000000u || type == 0x0a000000u)
			p += roundup2(vlen, 4);
		else if (type != 0x0b000000u && type != 0x0c000000u)
			return (0);					/* a collection inside: not needed */
	}
	return (0);
}

/* a boolean value (type 0x0b, the value is the low bit of the header) of a key: 1 when the key is there and true */
static int
ios_dict_true(const uint8_t *b, size_t n, const char *want)
{
	size_t p = 8;
	uint32_t cnt, i;

	if (n < 8 || le32(b, 0) != 0xd3)
		return (0);
	cnt = le32(b, 4) & 0x00ffffff;
	for (i = 0; i < cnt && p + 4 <= n; i++) {
		uint32_t kh = le32(b, p), klen = kh & 0x00ffffff, vh, type;
		bool match;

		p += 4;
		if ((kh & 0x7f000000u) != 0x08000000u || p + roundup2(klen, 4) + 4 > n)
			return (0);
		match = klen > 0 && strlen(want) + 1 == klen && strncmp((const char *)b + p, want, klen) == 0;
		p += roundup2(klen, 4);
		vh = le32(b, p); p += 4;
		type = vh & 0x7f000000u;
		if (type == 0x0b000000u) {
			if (match)
				return ((vh & 1) != 0);
		} else if (type == 0x04000000u)
			p += 8;
		else if (type == 0x09000000u || type == 0x08000000u || type == 0x0a000000u)
			p += roundup2(vh & 0x00ffffff, 4);
		else if (type != 0x0c000000u)
			return (0);
	}
	return (0);
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

/* the structure of 0xc68 bytes for a surface that is mapped in the task of td at va (with the region that the kernel keeps for the client) */
static int
ios_fill(struct thread *td, struct iosurf *sf, vm_offset_t va, uint8_t *out)
{
	vm_offset_t iva = 0;
	struct vm_object *io;
	uint64_t v, w = sf->w, h = sf->h, row = sf->row, fmt = sf->fmt, bpe = sf->bpe, alloc = sf->alloc;
	int rv;

	io = vm_object_allocate(OBJT_DEFAULT, 16);			/* the region that the kernel keeps for the client (the state of the surface: locks, seeds): zero pages */
	rv = ios_map(td->td_proc, io, 16 * PAGE_SIZE, &iva);
	vm_object_deallocate(io);
	if (rv != IOS_KERN_SUCCESS)
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
	return (0);
}

int
iosurface_create(struct thread *td, const uint8_t *props, size_t len, uint8_t *out)
{
	uint64_t w = 0, h = 0, bpe = 4, fmt = 0, row = 0, alloc = 0;
	struct iosurf *sf = NULL;
	vm_offset_t va = 0;
	int i;

	(void)iosurface_dict_number(props, len, "IOSurfaceWidth", &w);
	(void)iosurface_dict_number(props, len, "IOSurfaceHeight", &h);
	(void)iosurface_dict_number(props, len, "IOSurfaceBytesPerElement", &bpe);
	(void)iosurface_dict_number(props, len, "IOSurfacePixelFormat", &fmt);
	(void)iosurface_dict_number(props, len, "IOSurfaceBytesPerRow", &row);
	(void)iosurface_dict_number(props, len, "IOSurfaceAllocSize", &alloc);
	if (w == 0 || h == 0 || w > 16384 || h > 16384 || bpe == 0 || bpe > 16)
		return (IOS_NOTBADARG);
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
	sf->obj = vm_object_allocate(OBJT_DEFAULT, OFF_TO_IDX(round_page(alloc)));
	if (ios_map(td->td_proc, sf->obj, alloc, &va) != IOS_KERN_SUCCESS) {
		vm_object_deallocate(sf->obj);
		sf->used = false;
		return (IOS_NORESOURCES);
	}
	if (ios_fill(td, sf, va, out) != 0)
		return (IOS_NORESOURCES);
	if (darwin_debug >= 1)
		printf("darwinator: IOSurface %u made: %ux%u, row %u, %ju bytes, format %#x\n", sf->id, sf->w, sf->h, sf->row, (uintmax_t)alloc, sf->fmt);
	return (0);
}

/* lock (id, options, *seed), unlock (id, options, *seed), release (id): a surface is not locked against anybody here, the seed stays 0 */
int
iosurface_trap(struct thread *td, uint32_t index, uint64_t id, uint64_t options, uint64_t seed_user_addr)
{
	uint32_t seed = 0;

	(void)td; (void)id; (void)options;
	if (index == IOSURF_TRAP_LOCK || index == IOSURF_TRAP_UNLOCK) {
		if (seed_user_addr != 0 && copyout(&seed, (void *)seed_user_addr, sizeof(seed)) != 0)
			return (IOS_KERN_INVALID_ADDRESS);
		return (0);
	}
	if (index == IOSURF_TRAP_RELEASE)
		return (0);
	return (IOS_UNSUPPORTED);
}

/* selector 4: the surface of an id (the scalar) that another process made and marked global; the memory is mapped into the caller and the structure is the one of create.  kIOReturnBadArgument for an id
 * that is not there or not global: what the Mac answers */
int
iosurface_lookup(struct thread *td, uint64_t id, uint8_t *out)
{
	struct iosurf *sf = NULL;
	vm_offset_t va = 0;
	int i;

	mtx_lock(&ios_mtx);
	for (i = 0; i < IOS_MAX; i++)
		if (iosurfs[i].used && iosurfs[i].id == id && iosurfs[i].global) { sf = &iosurfs[i]; break; }
	mtx_unlock(&ios_mtx);
	if (sf == NULL)
		return (IOS_NOTBADARG);
	if (ios_map(td->td_proc, sf->obj, sf->alloc, &va) != IOS_KERN_SUCCESS)
		return (IOS_NORESOURCES);
	if (ios_fill(td, sf, va, out) != 0)
		return (IOS_NORESOURCES);
	return (0);
}
