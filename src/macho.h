/* The parts of the Mach-O format that the loader reads, written from the public format description. */
#ifndef _MACHO_H_
#define _MACHO_H_

#include <sys/types.h>

#define	MH_MAGIC_64		0xfeedfacfU
#define	FAT_MAGIC		0xcafebabeU		/* the fat header is big endian */

#define	CPU_ARCH_ABI64		0x01000000U
#define	CPU_TYPE_X86_64		(7U | CPU_ARCH_ABI64)
#define	CPU_TYPE_ARM64		(12U | CPU_ARCH_ABI64)

#define	MH_EXECUTE		2

#define	LC_REQ_DYLD		0x80000000U
#define	LC_SEGMENT_64		0x19
#define	LC_UNIXTHREAD		0x5
#define	LC_LOAD_DYLIB		0xc
#define	LC_LOAD_DYLINKER	0xe
#define	LC_MAIN			(0x28 | LC_REQ_DYLD)

#define	VMP_READ		1
#define	VMP_WRITE		2
#define	VMP_EXEC		4

/* thread states of LC_UNIXTHREAD */
#define	x86_THREAD_STATE64	4
#define	x86_THREAD_STATE64_COUNT 42		/* 32 bit words: 21 registers of 64 bits */
#define	X86_STATE64_RIP_INDEX	16		/* rax rbx rcx rdx rdi rsi rbp rsp r8-r15 rip */
#define	ARM_THREAD_STATE64	6
#define	ARM_THREAD_STATE64_COUNT 68		/* x0-x28 fp lr sp pc (64 bit), cpsr + pad (32 bit) */
#define	ARM_STATE64_PC_INDEX	32

struct fat_header {
	uint32_t magic;
	uint32_t nfat_arch;
};
struct fat_arch {
	uint32_t cputype;
	uint32_t cpusubtype;
	uint32_t offset;
	uint32_t size;
	uint32_t align;
};
struct mach_header_64 {
	uint32_t magic;
	uint32_t cputype;
	uint32_t cpusubtype;
	uint32_t filetype;
	uint32_t ncmds;
	uint32_t sizeofcmds;
	uint32_t flags;
	uint32_t reserved;
};
struct load_command {
	uint32_t cmd;
	uint32_t cmdsize;
};
struct segment_command_64 {
	uint32_t cmd;
	uint32_t cmdsize;
	char	 segname[16];
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	uint32_t maxprot;
	uint32_t initprot;
	uint32_t nsects;
	uint32_t flags;
};
struct entry_point_command {
	uint32_t cmd;
	uint32_t cmdsize;
	uint64_t entryoff;
	uint64_t stacksize;
};
struct thread_command {
	uint32_t cmd;
	uint32_t cmdsize;
	uint32_t flavor;
	uint32_t count;
	/* uint32_t state[count]; */
};

#endif
