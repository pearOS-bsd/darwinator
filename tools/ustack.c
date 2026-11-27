/* ustack: the user stacks of every thread of a Mach-O process that runs on darwinator (FreeBSD ptrace), for a process that hangs: where each thread waits.
 *   ustack PID [FRAMES]
 * It attaches, prints for each thread its id, rip and the return addresses of the rbp chain (absolute addresses: subtract the slide of the process to get the unslid address of the
 * dyld cache, then `ipsw dyld a2s`), and detaches; the process goes on.  Build on FreeBSD: cc -o ustack ustack.c */
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <machine/reg.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
peek(pid_t pid, uint64_t addr, uint64_t *out)
{
	errno = 0;
	long v = ptrace(PT_READ_D, pid, (caddr_t)addr, 0);
	if (errno != 0)
		return (-1);
	uint32_t lo = (uint32_t)v;
	errno = 0;
	long w = ptrace(PT_READ_D, pid, (caddr_t)(addr + 4), 0);
	if (errno != 0)
		return (-1);
	*out = (uint64_t)lo | ((uint64_t)(uint32_t)w << 32);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: ustack PID [FRAMES]\n");
		return (2);
	}
	pid_t pid = atoi(argv[1]);
	int frames = argc > 2 ? atoi(argv[2]) : 32, st;

	if (ptrace(PT_ATTACH, pid, 0, 0) != 0) {
		perror("PT_ATTACH");
		return (1);
	}
	waitpid(pid, &st, 0);
	int n = ptrace(PT_GETNUMLWPS, pid, 0, 0);
	lwpid_t *l = calloc(n > 0 ? n : 1, sizeof(*l));
	n = ptrace(PT_GETLWPLIST, pid, (caddr_t)l, n);
	for (int i = 0; i < n; i++) {
		struct reg r;

		if (ptrace(PT_GETREGS, l[i], (caddr_t)&r, 0) != 0) {
			printf("thread %d: no registers\n", l[i]);
			continue;
		}
		printf("thread %d: rip %#lx rsp %#lx rbp %#lx rdi %#lx rsi %#lx\n", l[i], (unsigned long)r.r_rip, (unsigned long)r.r_rsp, (unsigned long)r.r_rbp, (unsigned long)r.r_rdi, (unsigned long)r.r_rsi);
		uint64_t ra;
		if (peek(pid, r.r_rsp, &ra) == 0)
			printf("  [rsp] %#lx\n", (unsigned long)ra);	/* a leaf (the system call stubs have no frame): the caller is at the top of the stack */
		uint64_t fp = r.r_rbp;
		for (int f = 0; f < frames && fp != 0; f++) {
			uint64_t next, ret;
			if (peek(pid, fp, &next) != 0 || peek(pid, fp + 8, &ret) != 0)
				break;
			printf("  %#lx\n", (unsigned long)ret);
			if (next <= fp)
				break;
			fp = next;
		}
	}
	ptrace(PT_DETACH, pid, (caddr_t)1, 0);
	return (0);
}
