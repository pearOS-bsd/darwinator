/* mement: a memory entry of vm_allocate'd memory (what CoreAnimation does for a CAImageQueue's shared memory: CA::Render::Shmem::new_shmem
 * vm_allocates, the commit sends a memory entry of it) must share the pages: writes to the original address after the entry was made are
 * seen through a mapping of the entry, and the other way round.  Same output on the Mac and on pearOS.
 * Build on a Mac: clang mement.c -o mement */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
int main(void)
{
	vm_address_t a = 0; vm_size_t sz = 4096;
	kern_return_t kr = vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_ANYWHERE | VM_MAKE_TAG(0x33));
	volatile unsigned *p = (volatile unsigned *)a;
	p[0] = 0x11111111;					/* touched before the entry, as the queue's header is */
	memory_object_size_t esz = sz; mach_port_t ent = MACH_PORT_NULL;
	kr = kr ? kr : mach_make_memory_entry_64(mach_task_self(), &esz, (memory_object_offset_t)a, VM_PROT_READ | VM_PROT_WRITE, &ent, MACH_PORT_NULL);
	mach_vm_address_t b = 0;
	kr = kr ? kr : mach_vm_map(mach_task_self(), &b, sz, 0, VM_FLAGS_ANYWHERE, ent, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr) { printf("mement: kr %#x\n", kr); return 1; }
	volatile unsigned *q = (volatile unsigned *)b;
	p[0] = 0x22222222; p[100] = 0x33333333;		/* written after the entry was made */
	q[200] = 0x44444444;
	printf("mement: through the entry %#x %#x, through the original %#x: %s\n", q[0], q[100], p[200], q[0] == 0x22222222 && q[100] == 0x33333333 && p[200] == 0x44444444 ? "shared" : "NOT SHARED");
	return 0;
}
