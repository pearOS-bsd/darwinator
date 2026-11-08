/*
 * The BSD class of the Darwin system calls.  The numbers are the ones of xnu's syscalls.master; the calls
 * whose arguments and results are the same as FreeBSD's are routed to FreeBSD's implementation.  Calls that
 * differ (open flags, stat layouts, mmap flags, signals ...) get their own functions when they are written,
 * until then they fail with ENOSYS.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

#define	DSYS_exit	1
#define	DSYS_read	3
#define	DSYS_write	4
#define	DSYS_close	6
#define	DSYS_getpid	20
#define	DARWIN_NSYSCALLS 21		/* highest number used + 1; grows with the table */

#define	ENTRY(num, nargs, fn, ev) \
	[num] = { .sy_narg = (nargs), .sy_call = (sy_call_t *)(fn), .sy_auevent = (ev), .sy_thrcnt = SY_THR_STATIC }

struct sysent darwin_sysent[DARWIN_NSYSCALLS] = {
	ENTRY(DSYS_exit,   1, sys_exit, AUE_EXIT),
	ENTRY(DSYS_read,   3, sys_read,     AUE_READ),
	ENTRY(DSYS_write,  3, sys_write,    AUE_WRITE),
	ENTRY(DSYS_close,  1, sys_close,    AUE_CLOSE),
	ENTRY(DSYS_getpid, 0, sys_getpid,   AUE_GETPID),
};

const char *darwin_syscallnames[DARWIN_NSYSCALLS] = {
	[DSYS_exit] = "exit",
	[DSYS_read] = "read",
	[DSYS_write] = "write",
	[DSYS_close] = "close",
	[DSYS_getpid] = "getpid",
};

const int darwin_nsyscalls = DARWIN_NSYSCALLS;

void
darwin_syscalls_init(void)
{
	int i;

	for (i = 0; i < DARWIN_NSYSCALLS; i++) {
		if (darwin_sysent[i].sy_call == NULL) {
			darwin_sysent[i] = (struct sysent){ .sy_narg = 0, .sy_call = (sy_call_t *)nosys,
			    .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
			darwin_syscallnames[i] = "#unimplemented";
		}
	}
}
