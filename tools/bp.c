/* bp: a tiny breakpoint tool for a Mach-O process that runs on darwinator (FreeBSD ptrace): the only way to look inside the closed libraries on this system, as lldb does on a Mac.
 *   bp PID ADDR [ADDR ...]       ADDR are absolute addresses (the unslid address of the cache + the slide of the process)
 * It attaches, writes an int3 at each address, and at every hit prints the registers rdi rsi rdx rcx r8 r9 and the return address, restores the byte, single-steps, puts the int3 back and continues.
 * Output lines: "BP <addr> hit <n>: rdi=.. rsi=.. rdx=.. rcx=.. r8=.. r9=.. ret=..".  Detaches after 30 seconds or when the process ends.
 * BPWAIT=1: for a process stopped before its first instruction (sysctl kern.darwinator.stop=1): the addresses of the dyld cache are not mapped yet, so the
 * process runs in steps of 1 ms (continue, SIGSTOP) until the first address can be read; then the breakpoints go in (what happens in the first
 * milliseconds of a program can be watched: the real Dock checks in with launchservicesd before anything can attach to it). */
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <machine/reg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define MAXBP 16
static struct { uint64_t addr; long orig; int hits; } bp[MAXBP]; static int nbp;


static int memreg, countmode; static long memoff, memlen; static time_t lastsec;

int main(int argc, char **argv)
{
	countmode = getenv("BPCOUNT") != NULL;
	if (getenv("BPMEM")) { char rn[8]; if (sscanf(getenv("BPMEM"), "%7[a-z0-9]:%li:%li", rn, &memoff, &memlen) == 3) memreg = !strcmp(rn, "rdi") ? 1 : !strcmp(rn, "rsi") ? 2 : !strcmp(rn, "rdx") ? 3 : !strcmp(rn, "rcx") ? 4 : !strcmp(rn, "r8") ? 5 : 0; if (memlen > 256) memlen = 256; }
	pid_t pid; int st, i; struct reg r; time_t t0 = time(NULL);
	if (argc < 3) { fprintf(stderr, "usage: bp PID ADDR...\n"); return 2; }
	pid = atoi(argv[1]);
	for (i = 2; i < argc && nbp < MAXBP; i++) bp[nbp++].addr = strtoull(argv[i], NULL, 16);
	if (ptrace(PT_ATTACH, pid, 0, 0) != 0) { perror("attach"); return 1; }
	waitpid(pid, &st, 0);
	if (getenv("BPWAIT")) {
		int tries;

		for (tries = 0; tries < 20000; tries++) {
			struct ptrace_io_desc io; unsigned char b;
			io.piod_op = PIOD_READ_D; io.piod_offs = (void *)bp[0].addr; io.piod_addr = &b; io.piod_len = 1;
			if (ptrace(PT_IO, pid, (caddr_t)&io, 0) == 0 && io.piod_len == 1) break;
			ptrace(PT_CONTINUE, pid, (caddr_t)1, 0); usleep(1000); kill(pid, SIGSTOP); waitpid(pid, &st, 0);
			if (WIFEXITED(st) || WIFSIGNALED(st)) { fprintf(stderr, "bp: the process ended before the addresses were mapped\n"); return 1; }
		}
		fprintf(stderr, "bp: mapped after %d steps\n", tries);
	}
	for (i = 0; i < nbp; i++) {
		struct ptrace_io_desc io; unsigned char b;
		io.piod_op = PIOD_READ_D; io.piod_offs = (void *)bp[i].addr; io.piod_addr = &b; io.piod_len = 1;
		if (ptrace(PT_IO, pid, (caddr_t)&io, 0) != 0) { perror("read"); continue; }
		bp[i].orig = b; b = 0xcc; io.piod_op = PIOD_WRITE_D; io.piod_addr = &b;
		if (ptrace(PT_IO, pid, (caddr_t)&io, 0) != 0) perror("write");
	}
	fprintf(stderr, "bp: attached to %d with %d breakpoints\n", (int)pid, nbp);
	ptrace(PT_CONTINUE, pid, (caddr_t)1, 0);
	lastsec = time(NULL);
	while (time(NULL) - t0 < (getenv("BPSECS") ? atoi(getenv("BPSECS")) : 30)) {
		if (countmode && time(NULL) != lastsec) { lastsec = time(NULL); printf("T+%ld:", (long)(lastsec - t0)); for (int q = 0; q < nbp; q++) { printf(" %lx=%d", (unsigned long)(bp[q].addr & 0xfffff), bp[q].hits); bp[q].hits = 0; } printf("\n"); fflush(stdout); }
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == 0) { usleep(1000); continue; }
		if (w < 0 || WIFEXITED(st) || (WIFSIGNALED(st))) break;
		if (WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP) {
			ptrace(PT_GETREGS, pid, (caddr_t)&r, 0);
			uint64_t a = r.r_rip - 1; int k;
			for (k = 0; k < nbp; k++) if (bp[k].addr == a) break;
			if (k < nbp) {
				uint64_t ret = 0; struct ptrace_io_desc io; io.piod_op = PIOD_READ_D; io.piod_offs = (void *)r.r_rsp; io.piod_addr = &ret; io.piod_len = 8; ptrace(PT_IO, pid, (caddr_t)&io, 0);
				if (!countmode) { struct timespec tt; clock_gettime(CLOCK_MONOTONIC, &tt); printf("[%ld.%03ld] ", (long)(tt.tv_sec % 1000), tt.tv_nsec / 1000000); }
				++bp[k].hits; if (!countmode) printf("BP %lx hit %d: rdi=%lx rsi=%lx rdx=%lx rcx=%lx r8=%lx r9=%lx ret=%lx\n", (unsigned long)a, bp[k].hits, (unsigned long)r.r_rdi, (unsigned long)r.r_rsi, (unsigned long)r.r_rdx, (unsigned long)r.r_rcx, (unsigned long)r.r_r8, (unsigned long)r.r_r9, (unsigned long)ret);
				if (memreg && !countmode) { uint64_t base = memreg == 1 ? r.r_rdi : memreg == 2 ? r.r_rsi : memreg == 3 ? r.r_rdx : memreg == 4 ? r.r_rcx : r.r_r8; unsigned char mb[256]; struct ptrace_io_desc mio; mio.piod_op = PIOD_READ_D; mio.piod_offs = (void *)(base + memoff); mio.piod_addr = mb; mio.piod_len = memlen; if (ptrace(PT_IO, pid, (caddr_t)&mio, 0) == 0) { printf("   mem[%lx+%lx]:", (unsigned long)base, memoff); for (long q = 0; q < memlen; q++) printf("%s%02x", q % 8 == 0 ? " " : "", mb[q]); printf("\n"); } }
				fflush(stdout);
				unsigned char b = (unsigned char)bp[k].orig; io.piod_op = PIOD_WRITE_D; io.piod_offs = (void *)a; io.piod_addr = &b; io.piod_len = 1; ptrace(PT_IO, pid, (caddr_t)&io, 0);
				r.r_rip = a; ptrace(PT_SETREGS, pid, (caddr_t)&r, 0);
				ptrace(PT_STEP, pid, (caddr_t)1, 0); waitpid(pid, &st, 0);
				b = 0xcc; io.piod_offs = (void *)a; io.piod_addr = &b; ptrace(PT_IO, pid, (caddr_t)&io, 0);
				ptrace(PT_CONTINUE, pid, (caddr_t)1, 0);
				continue;
			}
		}
		ptrace(PT_CONTINUE, pid, (caddr_t)1, WIFSTOPPED(st) && WSTOPSIG(st) != SIGTRAP ? WSTOPSIG(st) : 0);
	}
	for (i = 0; i < nbp; i++) { struct ptrace_io_desc io; unsigned char b = (unsigned char)bp[i].orig; io.piod_op = PIOD_WRITE_D; io.piod_offs = (void *)bp[i].addr; io.piod_addr = &b; io.piod_len = 1; ptrace(PT_IO, pid, (caddr_t)&io, 0); }
	ptrace(PT_DETACH, pid, (caddr_t)1, 0);
	return 0;
}
