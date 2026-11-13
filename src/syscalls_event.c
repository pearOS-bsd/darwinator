/*
 * The BSD class, continued: kqueue/kevent/kevent64 (Darwin's event structures and filter numbers), the signal calls
 * (sigaction with its trampoline argument, sigaltstack, sigsuspend, sigpending), itimers, getfsstat64, mknod (the
 * device numbers of Darwin) and the AF_INET6 socket addresses (family 30 on Darwin, 28 here).
 *
 * Signals: a handler is called by FreeBSD's signal trampoline (the page the kernel maps for the process), with
 * the arguments the Darwin handler expects: (sig, siginfo*, ucontext*).  The siginfo has the same fields at the same
 * places up to si_addr; the ucontext is FreeBSD's, not Darwin's.  The trampoline address of Darwin's sigaction
 * (sa_tramp) is not used: the trampoline of the kernel returns with FreeBSD's sigreturn, which darwin_fetch_syscall_args
 * lets through.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/dirent.h>
#include <sys/unistd.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/sysctl.h>
#include <sys/filedesc.h>
#include <sys/capsicum.h>
#include <sys/vnode.h>
#include <sys/lock.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/socket.h>
#include <sys/mbuf.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"
#include "darwin_abi.h"

/* ---- mknod: dev_t of Darwin is major << 24 | minor (24 bits) ---- */

dev_t
darwin_to_dev(int32_t d)
{
	return (makedev((d >> 24) & 0xff, d & 0xffffff));
}

/* the device of the root file system is what a Mac says for its data volume (16777225 = 0x1000009), the number that the fsid of the statfs of that volume has: CarbonCore finds the volume of a file by it */
int32_t
darwin_from_dev(dev_t d)
{
	static dev_t root_dev = NODEV;
	static bool root_known;

	if (!root_known) {
		struct stat sb;

		if (kern_statat(curthread, 0, AT_FDCWD, "/", UIO_SYSSPACE, &sb) == 0) {
			root_dev = sb.st_dev;
			root_known = true;
		}
	}
	if (root_known && d == root_dev)
		return (16777225);
	return ((int32_t)((major(d) & 0xff) << 24 | (minor(d) & 0xffffff)));
}

static int
darwin_mknod(struct thread *td, struct darwin_args *uap)
{
	return (kern_mknodat(td, AT_FDCWD, (const char *)uap->a[0], UIO_USERSPACE, (int)uap->a[1],
	    darwin_to_dev((int32_t)uap->a[2])));
}

/* ---- getfsstat64(buf, bufsize, flags): the count, or the structures that fit ---- */

SYSCTL_DECL(_kern_darwinator);
#define	DA_CMNEXT_SUPPORTED	0x7fcu		/* ATTR_CMNEXT_: relpath 4, privatesize 8, linkid 0x10, nofirmlinkpath 0x20, realdevid 0x40, realfsid 0x80, cloneid 0x100, ext flags 0x200, recursive gencount 0x400 */
int darwin_two_volumes = 1;
SYSCTL_INT(_kern_darwinator, OID_AUTO, two_volumes, CTLFLAG_RW, &darwin_two_volumes, 0, "getfsstat shows the root as a system and a data volume");

static int
darwin_getfsstat64(struct thread *td, struct darwin_args *uap)
{
	struct statfs *sb = NULL;
	size_t count = 0, maxn = 0, i;
	int error, mode = (int)uap->a[2];
	char *ubuf = (char *)uap->a[0];

	if (ubuf != NULL && (int)uap->a[1] > 0)
		maxn = (size_t)(int)uap->a[1] / sizeof(struct darwin_statfs64);
	if (ubuf == NULL) {
		error = kern_getfsstat(td, &sb, 0, &count, UIO_SYSSPACE, mode == 1 ? MNT_WAIT : MNT_NOWAIT);
		if (error == 0 && count > 0 && darwin_two_volumes) {		/* the msdosfs mounts (the EFI partition) are left out: a Mac has none */
			size_t n2 = 0, k, drop = 0;
			struct statfs *sb2 = NULL;

			if (kern_getfsstat(td, &sb2, count * sizeof(struct statfs), &n2, UIO_SYSSPACE, MNT_NOWAIT) == 0) {
				for (k = 0; k < n2; k++)
					if (strcmp(sb2[k].f_fstypename, "msdosfs") == 0) drop++;
				free(sb2, M_STATFS);
			}
			count -= drop;
		}
		if (error == 0)
			td->td_retval[0] = count + (darwin_two_volumes == 2 ? 1 : darwin_two_volumes ? 5 : 0);		/* the root is six volumes (two with two_volumes=2: system and data) */
		return (error);
	}
	error = kern_getfsstat(td, &sb, (maxn > 5 && darwin_two_volumes ? maxn - (darwin_two_volumes == 2 ? 1 : 5) : maxn) * sizeof(struct statfs), &count, UIO_SYSSPACE,
	    mode == 1 ? MNT_WAIT : MNT_NOWAIT);
	if (error == 0) {
		size_t j = 0;

		for (i = 0; i < count && error == 0; i++) {
			int two = strcmp(sb[i].f_fstypename, "ufs") == 0 || strcmp(sb[i].f_fstypename, "zfs") == 0;

			if (j >= maxn)
				break;
			if (darwin_two_volumes && strcmp(sb[i].f_fstypename, "msdosfs") == 0)
				continue;
			error = darwin_statfs_out_vol(&sb[i], ubuf + j++ * sizeof(struct darwin_statfs64), two ? 1 : 0);
			if (two && darwin_two_volumes) {		/* the other volumes of the container, in the order of a Mac: Preboot, VM, Update, Data, Update/mnt1 */
				static const int order[] = { 3, 4, 5, 2, 6 }, order2[] = { 2 };
				const int *ord = darwin_two_volumes == 2 ? order2 : order;
				size_t k, nord = darwin_two_volumes == 2 ? nitems(order2) : nitems(order);

				for (k = 0; k < nord && error == 0 && j < maxn; k++)
					error = darwin_statfs_out_vol(&sb[i], ubuf + j++ * sizeof(struct darwin_statfs64), ord[k]);
			}
		}
		if (error == 0)
			td->td_retval[0] = j;
	}
	if (sb != NULL)
		free(sb, M_STATFS);
	return (error);
}

/* ---- kqueue, kevent, kevent64 ---- */

/* Darwin: struct kevent is 32 bytes, kevent64_s is 48 (two more extension words) */
struct dkev {
	uint64_t	ident;
	int16_t		filter;
	uint16_t	flags;
	uint32_t	fflags;
	int64_t		data;
	uint64_t	udata;
	uint64_t	ext[4];		/* kevent64 only (ext[1..3] only from kevent_qos: address, mask and expected value of the state of a workloop) */
};

struct dkev_arg {
	char	*in, *out;		/* the user arrays, advanced as the kernel takes and gives events */
	size_t	size;			/* 32 or 48 */
	bool	errors_only;		/* KEVENT_FLAG_ERROR_EVENTS: the changes are receipted, and only the receipts that report an error come back */
	int	nout;			/* the events that were really given in that mode */
	struct { uint64_t ident, udata; int16_t filter; uint16_t flags; } chg[64];	/* the changes of this call as they were given: an error comes back with the flags of its change (libdispatch looks for EV_DELETE in it before it touches the udata of a note that it has freed already) */
	int nchg;
	uint64_t dout, davail;		/* kevent_qos: the buffer in which the events of Mach ports bring their messages, and what is left of it */
};


/* Darwin's filters: -10 is EVFILT_USER (FreeBSD -11), -8 (Mach ports) and -12 (VM) have no counterpart */
static int
filter_from_darwin(int f)
{
	switch (f) {
	case -1: case -2: case -3: case -4: case -5: case -6: case -7: case -9:
		return (f);
	case -10:
		return (EVFILT_USER);
	case -8:
		return (EVFILT_EMPTY);		/* EVFILT_MACHPORT: the slot is taken by mach_ipc.c */
	default:
		if (darwin_debug >= 1)
			printf("darwinator: %s[%d]: kevent filter %d is not implemented\n", curthread->td_proc->p_comm, curthread->td_proc->p_pid, f);
		return (0x7000);		/* unknown to the kernel: the change comes back as an EV_ERROR event */
	}
}

static int
filter_to_darwin(int f)
{
	return (f == EVFILT_USER ? -10 : f == EVFILT_EMPTY ? -8 : f);
}

static uint32_t
timer_fflags_from_darwin(uint32_t f)
{
	uint32_t r = f & ~0x7;		/* NOTE_SECONDS 1 is the same; Darwin's USECONDS 2 and NSECONDS 4 are 4 and 8 here */

	if (f & 1) r |= NOTE_SECONDS;
	if (f & 2) r |= NOTE_USECONDS;
	if (f & 4) r |= NOTE_NSECONDS;
	return (r);
}


/*
 * The synchronisation of workloops (dispatch_sync on a busy workloop): a waiter makes EVFILT_WORKLOOP with NOTE_WL_SYNC_WAIT and its thread id as the ident, and the kernel keeps it until the owner wakes it
 * (NOTE_WL_SYNC_WAKE with the thread id of the waiter) or the waiter is ended (EV_DELETE with NOTE_WL_END_OWNERSHIP; ENOENT when it never waited).  A wake that comes first is kept (a "fake" waiter)
 * and the wait that follows returns at once.  The ownership of the workloop itself is not tracked: libdispatch keeps that state in userland.
 */
#define	NOTE_WL_SYNC_WAIT	0x4
#define	NOTE_WL_SYNC_WAKE	0x8
#define	NOTE_WL_END_OWNERSHIP	0x20
#define	SW_N	128
static struct { bool used; uint64_t tid; int state; } sw_tab[SW_N];		/* state: 0 waiting, 1 woken (it has not run yet), 2 a wake that came first, 3 owner (woken and running), 4 cancelled */
static struct mtx sw_mtx;
MTX_SYSINIT(darwin_sw_mtx, &sw_mtx, "darwin wl sync", MTX_DEF);

/* returns 0, or ENOENT for an end that finds no waiter */
static int
darwin_wl_sync(uint64_t tid, uint32_t fflags, uint16_t flags, uint64_t value)
{
	int i, free_i = -1, error = 0;

	mtx_lock(&sw_mtx);
	for (i = 0; i < SW_N; i++) {
		if (sw_tab[i].used && sw_tab[i].tid == tid)
			break;
		if (!sw_tab[i].used && free_i < 0)
			free_i = i;
	}
	if (i == SW_N)
		i = -1;
	if ((flags & EV_DELETE) != 0 && (fflags & NOTE_WL_SYNC_WAKE) != 0) {		/* the end: the knote of the waiter (or of the owner that it became) goes */
		if (value != 0) {		/* the owner ends its ownership (it passes the state of the queue that it knew; a waiter that is cancelled passes 0) */
			if (i >= 0 && sw_tab[i].state != 0) sw_tab[i].used = false;
		} else if (i < 0)
			error = ENOENT;						/* nothing: the waiter did not make its wait yet and no wake came */
		else if (sw_tab[i].state == 0) { sw_tab[i].state = 4; wakeup(sw_tab); }	/* cancelled while it waits: it goes out without ownership */
		else sw_tab[i].used = false;
	} else if ((fflags & NOTE_WL_SYNC_WAKE) != 0) {					/* a wake: the waiter goes on and its knote stays until the end; with no waiter yet the wake is kept */
		if (i >= 0 && sw_tab[i].state == 0) { sw_tab[i].state = 1; wakeup(sw_tab); }
		else if (i < 0 && free_i >= 0) { sw_tab[free_i].used = true; sw_tab[free_i].tid = tid; sw_tab[free_i].state = 2; }
	} else if ((fflags & NOTE_WL_SYNC_WAIT) != 0) {					/* the wait */
		if (i >= 0 && sw_tab[i].state == 2) sw_tab[i].state = 3;			/* the wake came first: owner now */
		else if (free_i >= 0 || i >= 0) {
			if (i < 0) { i = free_i; sw_tab[i].used = true; sw_tab[i].tid = tid; }
			sw_tab[i].state = 0;
			while (sw_tab[i].state == 0 && sw_tab[i].used) {
				if (msleep(sw_tab, &sw_mtx, PCATCH, "wlsync", hz / 4) == EINTR)
					break;
			}
			if (sw_tab[i].state == 1) sw_tab[i].state = 3;			/* woken: the knote of the owner stays */
			else sw_tab[i].used = false;					/* cancelled or interrupted */
		}
	}
	mtx_unlock(&sw_mtx);
	return (error);
}

static int
darwin_kev_copyin(void *argp, struct kevent *kevp, int count)
{
	struct dkev_arg *a = argp;
	struct dkev d;
	int i, error;

	for (i = 0; i < count; i++) {
		memset(&d, 0, sizeof(d));
		if (a->size == 72) {			/* kevent_qos_s: ident 0, filter 8, flags 10, qos 12, udata 16, fflags 24, xflags 28, data 32, ext[4] 40 */
			char raw[72];

			if ((error = copyin(a->in, raw, 72)) != 0)
				return (error);
			memcpy(&d.ident, raw, 8); memcpy(&d.filter, raw + 8, 2); memcpy(&d.flags, raw + 10, 2); memcpy(&d.udata, raw + 16, 8);
			memcpy(&d.fflags, raw + 24, 4); memcpy(&d.data, raw + 32, 8); memcpy(d.ext, raw + 40, 32);
		} else if ((error = copyin(a->in, &d, a->size)) != 0)
			return (error);
		a->in += a->size;
		memset(&kevp[i], 0, sizeof(kevp[i]));
		kevp[i].ident = d.ident;
		kevp[i].filter = filter_from_darwin(d.filter);
		kevp[i].flags = d.flags & 0xff;		/* EV_UDATA_SPECIFIC (0x100) has no counterpart; 0x100 here means something else */
		if (a->errors_only)
			kevp[i].flags |= EV_RECEIPT;
		kevp[i].fflags = kevp[i].filter == EVFILT_TIMER ? timer_fflags_from_darwin(d.fflags) : d.fflags;
		kevp[i].data = d.data;
		if (kevp[i].filter == EVFILT_TIMER) {
			/* Darwin's timer: the unit of data is SECONDS 1, USECONDS 2, NSECONDS 4, MACHTIME 0x100 (nanoseconds here) or milliseconds; ABSOLUTE (0x8) says that data is a deadline: in the uptime
			 * clock (mach_absolute_time) with MACHTIME, else on the wall clock (libdispatch's DISPATCH_CLOCK_WALL: nanoseconds since the epoch); LEEWAY, CRITICAL, BACKGROUND and
			 * MACH_CONTINUOUS_TIME (0x10, 0x20, 0x40, 0x80) are hints.  FreeBSD's kqueue gets a time from now in nanoseconds. */
			struct timespec nowts;
			int64_t now, unit, rel;

			unit = (d.fflags & 0x100) ? 1 : (d.fflags & 1) ? 1000000000LL : (d.fflags & 2) ? 1000LL : (d.fflags & 4) ? 1 : 1000000LL;
			if ((d.fflags & 0x8) != 0) {
				if ((d.fflags & 0x100) != 0) nanouptime(&nowts); else nanotime(&nowts);
				now = (int64_t)nowts.tv_sec * 1000000000LL + nowts.tv_nsec;
				rel = d.data * unit - now;
			} else
				rel = d.data * unit;
			kevp[i].fflags = NOTE_NSECONDS;
			kevp[i].data = rel > 0 ? rel : 1;
			{
				static int shown;

				if (darwin_debug >= 1 && shown < 600) {
					shown++;
					printf("darwinator: %s[%d]: timer change: ident %#lx flags %#x fflags %#x data %ld -> in %ld ns\n", curthread->td_proc->p_comm, curthread->td_proc->p_pid, (u_long)d.ident, d.flags, d.fflags, (long)d.data, (long)kevp[i].data);
				}
			}
		}
		kevp[i].udata = (void *)d.udata;
		if (a->nchg < (int)nitems(a->chg)) {
			a->chg[a->nchg].ident = kevp[i].ident; a->chg[a->nchg].udata = d.udata; a->chg[a->nchg].filter = kevp[i].filter; a->chg[a->nchg].flags = d.flags;
			a->nchg++;
		}
		kevp[i].ext[0] = d.ext[0];
		kevp[i].ext[1] = d.ext[1];
		if (d.filter == -17 && (d.fflags & 0x1) != 0 && ((d.flags & EV_DELETE) != 0 || (d.flags & EV_DISABLE) == 0))			/* NOTE_WL_THREAD_REQUEST: a thread for the workloop (or back with EV_DELETE); with EV_DISABLE (and NOTE_WL_UPDATE_QOS) it is only an update of the QoS of a request that exists */
		{
			/* XNU checks the state of the queue (ext: address of dq_state, mask, expected value) before it takes a request: when the queue has been taken by a thread already the request is stale */
			uint64_t qs = 0;
			bool stale = (d.flags & EV_DELETE) == 0 && d.ext[1] != 0 && d.ext[2] != 0 && copyin((void *)(uintptr_t)d.ext[1], &qs, 8) == 0 && (qs & d.ext[2]) != (d.ext[3] & d.ext[2]);

			if (darwin_debug >= 2)
				printf("darwinator: wl request %s %#lx fflags %#x flags %#x: ext addr %#lx mask %#lx want %#lx state %#lx\n", (d.flags & EV_DELETE) ? "delete" : "add", (u_long)d.ident, d.fflags, d.flags, (u_long)d.ext[1], (u_long)d.ext[2], (u_long)d.ext[3], (u_long)qs);
			if (stale && darwin_debug >= 2)
				printf("darwinator: stale thread request for the workloop %#lx: state %#lx mask %#lx want %#lx\n", (u_long)d.ident, (u_long)qs, (u_long)d.ext[2], (u_long)d.ext[3]);
			if (!stale)
				darwin_wq_request(curthread, (int64_t)d.ident, d.udata, 0, (d.flags & EV_DELETE) == 0, d.ext[1], d.ext[2], d.ext[3]);
		}
		if (d.filter == -17 && (d.fflags & (NOTE_WL_SYNC_WAIT | NOTE_WL_SYNC_WAKE)) != 0) {		/* the synchronisation of a workloop: done here (it may sleep); the kqueue gets a change that has no effect */
			int serr = darwin_wl_sync(d.ident, d.fflags, d.flags, d.ext[1]);

			kevp[i].filter = EVFILT_USER;
			kevp[i].ident = serr == 0 ? 0x57ffffff00000000ULL : 0x57fffffe00000000ULL;
			kevp[i].flags = (serr == 0 ? EV_ADD | EV_DISABLE : EV_DELETE) | (a->errors_only ? EV_RECEIPT : 0);	/* ENOENT: deleting what is not there gives it back as the error */
			kevp[i].fflags = 0;
			kevp[i].data = 0;
		} else
		if (d.filter == -17) {			/* EVFILT_WORKLOOP: the requests on a workloop (thread request, ownership) mean nothing here; a disabled EVFILT_USER takes their place, so that adds and deletes balance */
			kevp[i].filter = EVFILT_USER;
			kevp[i].ident = 0x57ffffff00000000ULL;			/* always the same disabled knote: an add that is repeated changes nothing and a delete never finds nothing */
			kevp[i].flags = EV_ADD | EV_DISABLE | (a->errors_only ? EV_RECEIPT : 0);
			kevp[i].fflags = 0;
			kevp[i].data = 0;
		}
		if (d.filter == -8) {			/* EVFILT_MACHPORT: the port, found now, goes with the change; the options of the receive in ext[3] */
			kevp[i].ext[3] = d.fflags;
			kevp[i].fflags = 0;
			if (d.flags & EV_ADD) {
				void *port = darwin_mach_port_for_event(curthread, (uint32_t)d.ident);

				if (darwin_debug >= 1)
					printf("darwinator: %s[%d]: EVFILT_MACHPORT add for name %#lx: %s (options %#x, flags %#x, udata %#lx)\n", curthread->td_proc->p_comm, curthread->td_proc->p_pid, (u_long)d.ident, port ? "found" : "no receive right with this name", d.fflags, d.flags, (u_long)d.udata);
				if (port == NULL)
					kevp[i].filter = 0x7000;	/* no such port (or no receive right): the change comes back as an error */
				else
					kevp[i].ext[2] = (uint64_t)(uintptr_t)port;
			}
		}
	}
	return (0);
}

static int
darwin_kev_copyout(void *argp, struct kevent *kevp, int count)
{
	struct dkev_arg *a = argp;
	struct dkev d;
	int i, error;

	for (i = 0; i < count; i++) {
		if (a->errors_only && (kevp[i].flags & EV_ERROR) != 0 && kevp[i].data == 0)
			continue;		/* a change that went well: a Mac says nothing */
		if (darwin_debug >= 1 && (kevp[i].flags & EV_ERROR) != 0)
			printf("darwinator: %s[%d]: kevent error event: ident %#lx filter %d flags %#x fflags %#x data %ld udata %p\n", curthread->td_proc->p_comm, curthread->td_proc->p_pid, (u_long)kevp[i].ident, kevp[i].filter, kevp[i].flags, kevp[i].fflags, (long)kevp[i].data, kevp[i].udata);
		memset(&d, 0, sizeof(d));
		d.ident = kevp[i].ident;
		d.filter = filter_to_darwin(kevp[i].filter);
		d.flags = kevp[i].flags;
		d.fflags = kevp[i].fflags;
		d.data = kevp[i].data;
		d.udata = (uint64_t)kevp[i].udata;
		d.ext[0] = kevp[i].ext[0];
		d.ext[1] = kevp[i].ext[1];
		if ((kevp[i].flags & EV_ERROR) != 0) {		/* an error: the flags of the change that failed, with EV_ERROR (as XNU gives it) */
			int q;

			for (q = 0; q < a->nchg; q++)
				if (a->chg[q].ident == kevp[i].ident && a->chg[q].filter == kevp[i].filter && a->chg[q].udata == (uint64_t)kevp[i].udata) {
					d.flags = a->chg[q].flags | EV_ERROR;
					break;
				}
		}
		if (kevp[i].filter == EVFILT_EMPTY)		/* EVFILT_MACHPORT: the notes are made with EV_UDATA_SPECIFIC by libdispatch (the flag has no counterpart here, so it is not kept): an event comes back with it, else libdispatch takes a one-shot note as one without it and deletes it again (ENOENT crashed it) */
			d.flags |= 0x0100;
		if (kevp[i].filter == EVFILT_EMPTY && (kevp[i].flags & EV_EOF) != 0)		/* a port that died: EV_VANISHED | EV_DELETE, as XNU gives it */
			d.flags = (d.flags & ~(uint16_t)EV_EOF) | 0x0200 /* EV_VANISHED */ | EV_DELETE;
		if (kevp[i].filter == EVFILT_EMPTY) {		/* EVFILT_MACHPORT: with MACH_RCV_MSG the message comes with the event (ext[0]: where, ext[1]: its size, fflags: the result of the receive) */
			uint32_t opt = (uint32_t)kevp[i].ext[3], got = 0;
			uint64_t buf = kevp[i].ext[0];
			uint32_t bsz = (uint32_t)kevp[i].ext[1];
			bool fromdout = false;
			int kr;

			d.fflags = 0;
			d.ext[0] = d.ext[1] = 0;
			if ((opt & 0x2) != 0 && (kevp[i].flags & EV_EOF) == 0) {
				if (buf == 0) { buf = a->dout; bsz = (uint32_t)MIN(a->davail, (uint64_t)0x7fffffff); fromdout = true; }
				kr = buf != 0 ? darwin_mach_event_receive(curthread, (uint32_t)kevp[i].ident, opt, buf, bsz, &got) : 0x10004004;
				d.fflags = (uint32_t)kr;
				d.ext[0] = buf; d.ext[1] = got;
				if (kr == 0 && fromdout) {
					uint64_t adv = ((uint64_t)got + 7) & ~7ULL;

					a->dout += adv; a->davail -= MIN(adv, a->davail);
				}
			}
		}
		if (darwin_debug >= 2 || (darwin_debug >= 1 && d.filter == -8))
			printf("darwinator: %s[%d]: event out: ident %#lx filter %d flags %#x fflags %#x data %#lx udata %#lx ext %#lx %#lx\n", curthread->td_proc->p_comm, curthread->td_proc->p_pid, (u_long)d.ident, d.filter, d.flags, d.fflags, (u_long)d.data, (u_long)d.udata, (u_long)d.ext[0], (u_long)d.ext[1]);
		if (a->size == 72) {
			char raw[72];

			memset(raw, 0, sizeof(raw));
			memcpy(raw, &d.ident, 8); memcpy(raw + 8, &d.filter, 2); memcpy(raw + 10, &d.flags, 2); memcpy(raw + 16, &d.udata, 8);
			memcpy(raw + 24, &d.fflags, 4); memcpy(raw + 32, &d.data, 8); memcpy(raw + 40, d.ext, 16);
			error = copyout(raw, a->out, 72);
		} else
			error = copyout(&d, a->out, a->size);
		if (error != 0)
			return (error);
		a->out += a->size;
		a->nout++;
	}
	return (0);
}

static int
darwin_kevent_common(struct thread *td, int kq, char *chg, int nchg, char *evt, int nevt, const void *utimeout,
    size_t size)
{
	struct dkev_arg a = { .in = chg, .out = evt, .size = size };
	struct kevent_copyops ops = { .arg = &a, .k_copyout = darwin_kev_copyout, .k_copyin = darwin_kev_copyin,
	    .kevent_size = sizeof(struct kevent) };
	struct timespec ts, *tsp = NULL;
	int error;

	if (utimeout != NULL) {
		error = copyin(utimeout, &ts, sizeof(ts));
		if (error != 0)
			return (error);
		tsp = &ts;
	}
	return (kern_kevent(td, kq, nchg, nevt, &ops, tsp));
}

static int
darwin_kevent(struct thread *td, struct darwin_args *uap)
{
	return (darwin_kevent_common(td, (int)uap->a[0], (char *)uap->a[1], (int)uap->a[2], (char *)uap->a[3],
	    (int)uap->a[4], (const void *)uap->a[5], 32));
}

static int
darwin_kevent64(struct thread *td, struct darwin_args *uap)
{
	return (darwin_kevent_common(td, (int)uap->a[0], (char *)uap->a[1], (int)uap->a[2], (char *)uap->a[3],
	    (int)uap->a[4], (const void *)uap->a[6], 48));
}


/* kevent_qos(fd, changelist, nchanges, eventlist, nevents, data_out, data_available, flags) and kevent_id(id, ... same): the events are 72 bytes; fd -1 and the ids of workloops are
 * kqueues of the process that are made when first used (a descriptor that the program does not know of).  There is no timeout: KEVENT_FLAG_IMMEDIATE (1) asks not to wait. */
static struct { struct proc *p; pid_t pid; int64_t id; int fd; } kq_tab[1024];
static int kq_hi;			/* the slots in use are all below this: the scans stop there (a thread of the pool looks at them at every wake-up) */
static struct mtx kq_mtx;
MTX_SYSINIT(darwin_kq, &kq_mtx, "darwin kq", MTX_DEF);

static int
kq_for(struct thread *td, int64_t id, int *fdp)
{
	struct proc *p = td->td_proc;
	int i, free_i = -1, error;
	struct kqueue_args ka;

	mtx_lock(&kq_mtx);
	for (i = 0; i < kq_hi; i++) {
		if (kq_tab[i].p == p && kq_tab[i].pid == p->p_pid && kq_tab[i].id == id) {
			*fdp = kq_tab[i].fd;
			mtx_unlock(&kq_mtx);
			return (0);
		}
		if (kq_tab[i].p == NULL && free_i < 0)
			free_i = i;
	}
	if (free_i < 0 && kq_hi < (int)nitems(kq_tab))
		free_i = kq_hi;
	mtx_unlock(&kq_mtx);
	if (free_i < 0)
		return (EMFILE);
	if ((error = sys_kqueue(td, &ka)) != 0)
		return (error);
	mtx_lock(&kq_mtx);
	kq_tab[free_i].p = p; kq_tab[free_i].pid = p->p_pid; kq_tab[free_i].id = id; kq_tab[free_i].fd = td->td_retval[0];
	if (free_i >= kq_hi)
		kq_hi = free_i + 1;
	*fdp = td->td_retval[0];
	mtx_unlock(&kq_mtx);
	return (0);
}

/* the process is gone: its kqueues (closed by the exit) are forgotten; without this the 64 slots fill up after 64 processes and kevent_qos fails with EMFILE */
void
darwin_kq_exit(struct proc *p)
{
	int i;

	mtx_lock(&kq_mtx);
	for (i = 0; i < kq_hi; i++)
		if (kq_tab[i].p == p)
			memset(&kq_tab[i], 0, sizeof(kq_tab[i]));
	while (kq_hi > 0 && kq_tab[kq_hi - 1].p == NULL)
		kq_hi--;
	mtx_unlock(&kq_mtx);
}

static int
darwin_kevent_qos_common(struct thread *td, struct darwin_args *uap, bool byid)
{
	int fd = byid ? 0 : (int)uap->a[0], error;
	uint32_t flags = (uint32_t)uap->a[7];
	struct timespec zero = { 0, 0 };
	struct dkev_arg a = { .in = (char *)uap->a[1], .out = (char *)uap->a[3], .size = 72 };
	uint64_t davail0 = 0;
	int nev;
	struct kevent_copyops ops = { .arg = &a, .k_copyout = darwin_kev_copyout, .k_copyin = darwin_kev_copyin, .kevent_size = sizeof(struct kevent) };

	if (darwin_debug >= 2) {
		int16_t f0 = 0, fl0 = 0;
		uint32_t ff0 = 0;
		uint64_t id0 = 0;
		int c;

		for (c = 0; c < (int)uap->a[2] && c < 4; c++) {
			(void)copyin(a.in + c * 72 + 8, &f0, sizeof(f0));
			(void)copyin(a.in + c * 72 + 10, &fl0, sizeof(fl0));
			(void)copyin(a.in + c * 72 + 24, &ff0, sizeof(ff0));
			(void)copyin(a.in + c * 72, &id0, sizeof(id0));
			printf("darwinator: %s[%d]: kevent_%s(%#lx, nchanges %d, nevents %d, flags %#x): change %d: ident %#lx filter %d flags %#x fflags %#x\n", td->td_proc->p_comm, td->td_proc->p_pid, byid ? "id" : "qos", (u_long)uap->a[0], (int)uap->a[2], (int)uap->a[4], flags, c, (u_long)id0, f0, fl0, ff0);
		}
	}
	if (byid || fd == -1) {
		if ((error = kq_for(td, byid ? (int64_t)uap->a[0] : -1, &fd)) != 0)
			return (error);
	}
	if (uap->a[5] != 0 && uap->a[6] != 0 && copyin((void *)uap->a[6], &davail0, sizeof(davail0)) == 0) {
		a.dout = uap->a[5]; a.davail = davail0;
	}
	/* KEVENT_FLAG_ERROR_EVENTS (2): the list only takes the errors of the changes, no events; and the events of the kqueue of the workqueue (fd -1) go to the threads of the pool only, never to a caller */
	a.errors_only = (flags & 2) != 0;
	nev = (!byid && (int)uap->a[0] == -1 && !a.errors_only) ? 0 : (int)uap->a[4];
	if (a.errors_only)
		nev = MAX(nev, (int)uap->a[2]);		/* room for a receipt of each change; the calls that bring only errors do not wait */
	error = kern_kevent(td, fd, (int)uap->a[2], nev, &ops, (flags & 1) || a.errors_only ? &zero : NULL);
	if (error == 0 && a.errors_only)
		td->td_retval[0] = a.nout;
	if (a.dout != 0 && a.davail != davail0)
		(void)copyout(&a.davail, (void *)uap->a[6], sizeof(a.davail));
	if (error == 0 && (byid || (int)uap->a[0] == -1) && (int)uap->a[2] > 0)
		darwin_wq_note(td);		/* sources were added to the kqueue of the workqueue: a thread of the pool has to look at it */
	if (error != 0 && darwin_debug >= 1)
		printf("darwinator: %s[%d]: kevent_%s(%d, nchanges %d, nevents %d, flags %#x) failed: %d\n", td->td_proc->p_comm, td->td_proc->p_pid, byid ? "id" : "qos", fd, (int)uap->a[2], (int)uap->a[4], flags, error);
	return (error);
}

/* the events of the kqueue of the workqueue for a thread of the pool: its changes (kevent list) are applied, up to nout events come back in the memory at out (on the stack of the thread), the messages of the Mach ports in the buffer at dout */
int
darwin_wq_kevent(struct thread *td, int64_t kqid, uint64_t in, int nin, uint64_t out, int nout, uint64_t dout, uint64_t *davail, int *count)
{
	struct timespec zero = { 0, 0 };
	struct dkev_arg a = { .in = (char *)in, .out = (char *)out, .size = 72, .dout = dout, .davail = *davail };
	struct kevent_copyops ops = { .arg = &a, .k_copyout = darwin_kev_copyout, .k_copyin = darwin_kev_copyin, .kevent_size = sizeof(struct kevent) };
	int fd, error;

	*count = 0;
	if ((error = kq_for(td, kqid, &fd)) != 0)
		return (error);
	error = kern_kevent(td, fd, nin, nout, &ops, &zero);
	if (error == 0)
		*count = td->td_retval[0];
	*davail = a.davail;
	return (error);
}

/* the ids of the workloops (kqueues made with kevent_id) of the process, for the threads of the pool that look for events */
int
darwin_wq_kq_ids(struct proc *p, int64_t *ids, int max)
{
	int i, n = 0;

	mtx_lock(&kq_mtx);
	for (i = 0; i < kq_hi && n < max; i++)
		if (kq_tab[i].p == p && kq_tab[i].pid == p->p_pid && kq_tab[i].id != -1)
			ids[n++] = kq_tab[i].id;
	mtx_unlock(&kq_mtx);
	return (n);
}

static int darwin_kevent_qos(struct thread *td, struct darwin_args *uap) { return (darwin_kevent_qos_common(td, uap, false)); }
static int darwin_kevent_id(struct thread *td, struct darwin_args *uap) { return (darwin_kevent_qos_common(td, uap, true)); }

/* ---- signals ---- */

struct darwin_sigaction {	/* what the sigaction call takes: with the trampoline */
	uint64_t	handler;
	uint64_t	tramp;
	uint32_t	mask;
	int32_t		flags;
};

struct darwin_osigaction {	/* what it gives back */
	uint64_t	handler;
	uint32_t	mask;
	int32_t		flags;
};

static void
mask_from_darwin(uint32_t m, sigset_t *set)
{
	int i;

	SIGEMPTYSET(*set);
	for (i = 1; i <= 32; i++)
		if (m & (1U << (i - 1)))
			SIGADDSET(*set, i);
}

static uint32_t
mask_to_darwin(const sigset_t *set)
{
	uint32_t m = 0;
	int i;

	for (i = 1; i <= 32; i++)
		if (SIGISMEMBER(*set, i))
			m |= 1U << (i - 1);
	return (m);
}

static int
darwin_sigaction(struct thread *td, struct darwin_args *uap)
{
	struct darwin_sigaction dn;
	struct darwin_osigaction dold;
	struct sigaction nact, oact, *np = NULL;
	int error, sig = (int)uap->a[0];

	if (sig <= 0 || sig > 32)
		return (EINVAL);
	if (uap->a[1] != 0) {
		error = copyin((void *)uap->a[1], &dn, sizeof(dn));
		if (error != 0)
			return (error);
		memset(&nact, 0, sizeof(nact));
		nact.sa_handler = (void (*)(int))dn.handler;
		nact.sa_flags = dn.flags & 0xff;	/* SA_ONSTACK ... SA_SIGINFO have the same values; Darwin's SA_USERTRAMP and SA_64REGSET (0x100, 0x200) are internal */
		mask_from_darwin(dn.mask, &nact.sa_mask);
		np = &nact;
	}
	error = kern_sigaction(td, sig, np, &oact, 0);
	if (error != 0)
		return (error);
	if (uap->a[2] != 0) {
		dold.handler = (uint64_t)oact.sa_handler;
		dold.mask = mask_to_darwin(&oact.sa_mask);
		dold.flags = oact.sa_flags & 0xff;
		error = copyout(&dold, (void *)uap->a[2], sizeof(dold));
	}
	return (error);
}

static int
darwin_sigsuspend(struct thread *td, struct darwin_args *uap)	/* the mask is the argument itself (32 bits) */
{
	sigset_t set;

	int error;
	static int shown;

	mask_from_darwin((uint32_t)uap->a[0], &set);
	error = kern_sigsuspend(td, set);
	if (darwin_debug >= 1 && shown < 12) {
		shown++;
		printf("darwinator: %s[%d]: sigsuspend(mask %#x) returned %d, pending %#x %#x %#x %#x, thread mask %#x\n", td->td_proc->p_comm, td->td_proc->p_pid, (uint32_t)uap->a[0], error,
		    td->td_siglist.__bits[0], td->td_proc->p_siglist.__bits[0], td->td_sigmask.__bits[0], td->td_proc->p_sigacts->ps_sigignore.__bits[0], td->td_sigmask.__bits[0]);
	}
	return (error);
}

static int
darwin_sigpending(struct thread *td, struct darwin_args *uap)
{
	sigset_t pending;
	uint32_t m;
	struct proc *p = td->td_proc;

	PROC_LOCK(p);
	pending = td->td_siglist;
	SIGSETOR(pending, p->p_siglist);
	PROC_UNLOCK(p);
	m = mask_to_darwin(&pending);
	return (copyout(&m, (void *)uap->a[0], sizeof(m)));
}

/* ---- itimers: the structures are the same ---- */

static int
darwin_setitimer(struct thread *td, struct darwin_args *uap)
{
	struct itimerval nv, ov;
	int error;

	if (uap->a[1] == 0)
		return (EFAULT);
	error = copyin((void *)uap->a[1], &nv, sizeof(nv));
	if (error != 0)
		return (error);
	/* Darwin's timeval has a 32 bit tv_usec and four bytes of padding after it (not zero in what alarm() builds): FreeBSD's is 64 bits, and a garbage upper half made the call fail with EINVAL */
	nv.it_interval.tv_usec = (int32_t)nv.it_interval.tv_usec;
	nv.it_value.tv_usec = (int32_t)nv.it_value.tv_usec;
	error = kern_setitimer(td, (u_int)uap->a[0], &nv, &ov);
	if (error == 0 && uap->a[2] != 0)
		error = copyout(&ov, (void *)uap->a[2], sizeof(ov));
	return (error);
}

static int
darwin_getitimer(struct thread *td, struct darwin_args *uap)
{
	struct itimerval v;
	int error = kern_getitimer(td, (u_int)uap->a[0], &v);

	return (error != 0 ? error : copyout(&v, (void *)uap->a[1], sizeof(v)));
}

/* ---- AF_INET6: the family byte of a socket address is 30 on Darwin, 28 here ---- */

static int
darwin_sockaddr_in(struct thread *td, struct darwin_args *uap, struct sockaddr **sa)
{
	struct sockaddr *s;
	int error;

	error = getsockaddr(&s, (const struct sockaddr *)uap->a[1], (size_t)uap->a[2]);
	if (error != 0)
		return (error);
	if (s->sa_family == DARWIN_AF_INET6)
		s->sa_family = AF_INET6;
	*sa = s;
	return (0);
}

static int
darwin_bind(struct thread *td, struct darwin_args *uap)
{
	struct sockaddr *sa;
	int error = darwin_sockaddr_in(td, uap, &sa);

	if (error != 0)
		return (error);
	error = kern_bindat(td, AT_FDCWD, (int)uap->a[0], sa);
	free(sa, M_SONAME);
	return (error);
}

static int
darwin_connect(struct thread *td, struct darwin_args *uap)
{
	struct sockaddr *sa;
	int error = darwin_sockaddr_in(td, uap, &sa);

	if (error != 0)
		return (error);
	error = kern_connectat(td, AT_FDCWD, (int)uap->a[0], sa);
	free(sa, M_SONAME);
	return (error);
}

/* after a call that gave an address back (accept, getsockname, getpeername, recvfrom): family 28 -> 30 */
static void
darwin_fix_family_out(void *uaddr, void *ulen)
{
	uint32_t len;
	uint8_t fam;

	if (uaddr == NULL || ulen == NULL || copyin(ulen, &len, sizeof(len)) != 0 || len < 2)
		return;
	if (copyin((char *)uaddr + 1, &fam, 1) == 0 && fam == AF_INET6) {
		fam = DARWIN_AF_INET6;
		(void)copyout(&fam, (char *)uaddr + 1, 1);
	}
}

static int
darwin_accept(struct thread *td, struct darwin_args *uap)
{
	int error = sys_accept(td, (struct accept_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[1], (void *)uap->a[2]);
	return (error);
}

static int
darwin_getsockname(struct thread *td, struct darwin_args *uap)
{
	int error = sys_getsockname(td, (struct getsockname_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[1], (void *)uap->a[2]);
	return (error);
}

static int
darwin_getpeername(struct thread *td, struct darwin_args *uap)
{
	int error = sys_getpeername(td, (struct getpeername_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[1], (void *)uap->a[2]);
	return (error);
}

static int
darwin_recvfrom(struct thread *td, struct darwin_args *uap)
{
	int error = sys_recvfrom(td, (struct recvfrom_args *)uap);

	if (error == 0)
		darwin_fix_family_out((void *)uap->a[4], (void *)uap->a[5]);
	return (error);
}


/* ---- getattrlist(path, attrlist, buffer, size, options): the common and file attributes that come from stat ---- */

struct darwin_attrlist {
	uint16_t	bitmapcount, reserved;
	uint32_t	commonattr, volattr, dirattr, fileattr, forkattr;
};

#define	DA_CMN_NAME		0x00000001
#define	DA_CMN_DEVID		0x00000002
#define	DA_CMN_FSID		0x00000004
#define	DA_CMN_OBJTYPE		0x00000008
#define	DA_CMN_OBJTAG		0x00000010
#define	DA_CMN_OBJID		0x00000020
#define	DA_CMN_PAROBJID		0x00000080
#define	DA_CMN_SCRIPT		0x00000100
#define	DA_CMN_CRTIME		0x00000200
#define	DA_CMN_MODTIME		0x00000400
#define	DA_CMN_CHGTIME		0x00000800
#define	DA_CMN_ACCTIME		0x00001000
#define	DA_CMN_OWNERID		0x00008000
#define	DA_CMN_GRPID		0x00010000
#define	DA_CMN_ACCESSMASK	0x00020000
#define	DA_CMN_FLAGS		0x00040000
#define	DA_CMN_FILEID		0x02000000
#define	DA_CMN_SUPPORTED	(DA_CMN_NAME | DA_CMN_DEVID | DA_CMN_FSID | DA_CMN_OBJTYPE | DA_CMN_OBJTAG | DA_CMN_OBJID | DA_CMN_PAROBJID | DA_CMN_SCRIPT | \
	DA_CMN_CRTIME | DA_CMN_MODTIME | DA_CMN_CHGTIME | DA_CMN_ACCTIME | DA_CMN_OWNERID | DA_CMN_GRPID | \
	DA_CMN_ACCESSMASK | DA_CMN_FLAGS | DA_CMN_FILEID)
#define	DA_CMN_GENCOUNT		0x00080000
#define	DA_CMN_DOCID		0x00100000
#define	DA_CMN_USERACCESS	0x00200000
#define	DA_CMN_PARENTID		0x04000000
#define	DA_CMN_FULLPATH		0x08000000
#define	DA_CMN_ADDEDTIME	0x10000000
#define	DA_CMN_ERROR		0x20000000
#define	DA_CMN_DATAPROT		0x40000000
#define	DA_CMN_RETURNED		0x80000000
#define	DA_CMN_SUPPORTED_EXT	(DA_CMN_SUPPORTED | DA_CMN_GENCOUNT | DA_CMN_DOCID | DA_CMN_USERACCESS | DA_CMN_PARENTID | DA_CMN_FULLPATH | DA_CMN_ADDEDTIME | DA_CMN_ERROR | DA_CMN_DATAPROT | DA_CMN_RETURNED)
#define	DA_DIR_SUPPORTED	0x00000007	/* link count, entry count, mount status */
#define	DA_FILE_LINKCOUNT	0x00000001
#define	DA_FILE_TOTALSIZE	0x00000002
#define	DA_FILE_ALLOCSIZE	0x00000004
#define	DA_FILE_IOBLOCKSIZE	0x00000008
#define	DA_FILE_DATALENGTH	0x00000200
#define	DA_FILE_DATAALLOCSIZE	0x00000400
#define	DA_FILE_SUPPORTED	(DA_FILE_LINKCOUNT | DA_FILE_TOTALSIZE | DA_FILE_ALLOCSIZE | DA_FILE_IOBLOCKSIZE | \
	DA_FILE_DATALENGTH | DA_FILE_DATAALLOCSIZE)

/* the attributes of a volume (ATTR_VOL_*): LaunchServices tells the volume of a file by its UUID, and drops what it cannot place.  The UUIDs are those of a Mac's
 * system volume (what is under /System, /usr, /bin, /sbin and the root) and data volume (the rest), so that a LaunchServices database made on a Mac fits */
#define	DA_VOL_FSTYPE		0x00000001
#define	DA_VOL_SIGNATURE	0x00000002
#define	DA_VOL_SIZE		0x00000004
#define	DA_VOL_SPACEFREE	0x00000008
#define	DA_VOL_SPACEAVAIL	0x00000010
#define	DA_VOL_MINALLOC		0x00000020
#define	DA_VOL_ALLOCCLUMP	0x00000040
#define	DA_VOL_IOBLOCKSIZE	0x00000080
#define	DA_VOL_MOUNTPOINT	0x00001000
#define	DA_VOL_NAME		0x00002000
#define	DA_VOL_MOUNTFLAGS	0x00004000
#define	DA_VOL_MOUNTEDDEVICE	0x00008000
#define	DA_VOL_ENCODINGSUSED	0x00010000
#define	DA_VOL_CAPABILITIES	0x00020000
#define	DA_VOL_UUID		0x00040000
#define	DA_VOL_ATTRIBUTES	0x40000000
#define	DA_VOL_OBJCOUNT		0x00000100
#define	DA_VOL_FILECOUNT	0x00000200
#define	DA_VOL_DIRCOUNT		0x00000400
#define	DA_VOL_INFO		0x80000000
#define	DA_VOL_SUPPORTED	(DA_VOL_INFO | DA_VOL_OBJCOUNT | DA_VOL_FILECOUNT | DA_VOL_DIRCOUNT | DA_VOL_FSTYPE | DA_VOL_SIGNATURE | DA_VOL_MINALLOC | DA_VOL_ALLOCCLUMP | DA_VOL_MOUNTFLAGS | DA_VOL_MOUNTEDDEVICE | DA_VOL_ENCODINGSUSED | DA_VOL_SIZE | DA_VOL_SPACEFREE | DA_VOL_SPACEAVAIL | DA_VOL_IOBLOCKSIZE | DA_VOL_MOUNTPOINT | DA_VOL_NAME | DA_VOL_CAPABILITIES | DA_VOL_UUID | DA_VOL_ATTRIBUTES)

/* the volumes of the container of a Mac that has a sealed system: 1 system, 2 data, 3 Preboot, 4 VM, 5 Update, 6 Update/mnt1 (the values of a Mac VM) */
static const struct { const char *on, *name, *dev; uint8_t uuid[16]; uint32_t flags; } da_vols[] = {
	{ 0, 0, 0, { 0 }, 0 },
	{ "/", "Untitled", "/dev/disk3s4s1", { 0x2B, 0x58, 0x8F, 0x62, 0x3F, 0x2E, 0x49, 0x10, 0xAC, 0xDC, 0xB6, 0xC1, 0x03, 0x76, 0x68, 0xEB }, 0x4480d001 },
	{ "/System/Volumes/Data", "Untitled - Data", "/dev/disk3s1", { 0xFF, 0xB5, 0x71, 0x33, 0x80, 0x43, 0x40, 0x2E, 0xB8, 0xA2, 0x2F, 0xE7, 0x03, 0xF2, 0x07, 0x68 }, 0x4909000 },
	{ "/System/Volumes/Preboot", "Preboot", "/dev/disk3s2", { 0xF7, 0x9C, 0x77, 0xB2, 0x1E, 0x80, 0x4C, 0x02, 0x87, 0x85, 0x0B, 0x99, 0x7A, 0xE7, 0x88, 0x8A }, 0x4909000 },
	{ "/System/Volumes/VM", "VM", "/dev/disk3s6", { 0x91, 0xAC, 0xE9, 0x1F, 0xEB, 0xBB, 0x46, 0x18, 0xBD, 0x00, 0x5A, 0xB3, 0xFB, 0x85, 0x10, 0x37 }, 0x14909004 },
	{ "/System/Volumes/Update", "Update", "/dev/disk3s5", { 0x83, 0xD4, 0xE6, 0xC1, 0xA2, 0xF2, 0x4B, 0xE7, 0x97, 0xAF, 0x21, 0xBA, 0x27, 0xD1, 0x20, 0x65 }, 0x4909000 },
	{ "/System/Volumes/Update/mnt1", "Untitled", "/dev/disk3s4", { 0x10, 0x09, 0x14, 0x7C, 0xD2, 0x05, 0x45, 0x99, 0xA9, 0x07, 0xA2, 0xDE, 0x53, 0x0D, 0xE0, 0x89 }, 0x4909000 },
};

static const uint32_t da_fsid[7] = { 0, 16777231, 16777225, 16777226, 16777229, 16777230, 16777228 };

static int
da_vol_of_path(const char *path)
{
	if (strncmp(path, "/System/Volumes/Preboot", 23) == 0) return (3);
	if (strncmp(path, "/System/Volumes/VM", 18) == 0) return (4);
	if (strncmp(path, "/System/Volumes/Update/mnt1", 27) == 0) return (6);
	if (strncmp(path, "/System/Volumes/Update", 22) == 0) return (5);
	if (strncmp(path, "/System/Volumes/Data", 20) == 0) return (2);
	return (strcmp(path, "/") == 0 || strncmp(path, "/System", 7) == 0 || strncmp(path, "/usr", 4) == 0 || strncmp(path, "/bin", 4) == 0 || strncmp(path, "/sbin", 5) == 0 ? 1 : 2);
}

static bool
da_on_system_volume(const char *path)
{
	return (strcmp(path, "/") == 0 || strncmp(path, "/System", 7) == 0 || strncmp(path, "/usr", 4) == 0 || strncmp(path, "/bin", 4) == 0 || strncmp(path, "/sbin", 5) == 0);
}

static size_t attr_last_len;			/* the length of what the last attr_fill produced (for getattrlistbulk) */

static int
attr_fill(struct thread *td, struct darwin_args *uap, const struct stat *sbp, const char *path)
{
	struct darwin_attrlist al;
	struct stat sb = *sbp;
	char *out, *p;
	const char *name;
	size_t bufsize = (size_t)uap->a[3], len, namelen, copy;
	uint32_t cmn, file, objtype;
	int error;

	error = copyin((void *)uap->a[1], &al, sizeof(al));
	if (error != 0)
		return (error);
	if (al.bitmapcount != 5)
		return (EINVAL);
	cmn = al.commonattr;
	file = al.fileattr;
	if (darwin_debug >= 1 && (uap->a[4] & 0x20))
		printf("darwinator: %s[%d]: getattrlist %s extended: options %#lx common %#x vol %#x dir %#x file %#x fork %#x bufsize %lu\n", td->td_proc->p_comm, td->td_proc->p_pid, path, (u_long)uap->a[4], al.commonattr, al.volattr, al.dirattr, al.fileattr, al.forkattr, (u_long)uap->a[3]);
	if (cmn & DA_CMN_RETURNED) {			/* the caller takes what there is: the attributes that are not known are left out and the set that came back says which they are */
		cmn &= DA_CMN_SUPPORTED_EXT;
		al.volattr &= DA_VOL_SUPPORTED;
		al.dirattr = S_ISDIR(sb.st_mode) ? (al.dirattr & DA_DIR_SUPPORTED) : 0;
		file = S_ISDIR(sb.st_mode) ? 0 : (file & DA_FILE_SUPPORTED);
		al.forkattr = (uap->a[4] & 0x20) ? (al.forkattr & DA_CMNEXT_SUPPORTED) : 0;		/* FSOPT_ATTR_CMN_EXTENDED */
	}
	if ((cmn & ~DA_CMN_SUPPORTED_EXT) || (al.volattr & ~DA_VOL_SUPPORTED) || ((al.dirattr & ~DA_DIR_SUPPORTED) || (al.dirattr && !S_ISDIR(sb.st_mode) && !(cmn & DA_CMN_RETURNED))) || (al.forkattr & ~((uap->a[4] & 0x20) ? DA_CMNEXT_SUPPORTED : 0u)) || (file & ~DA_FILE_SUPPORTED)) {
		if (darwin_debug >= 1)
			printf("darwinator: %s[%d]: getattrlist %s: attributes not supported: common %#x (of which %#x), volume %#x, dir %#x, file %#x, fork %#x\n", td->td_proc->p_comm, td->td_proc->p_pid, path, cmn, cmn & ~DA_CMN_SUPPORTED, al.volattr, al.dirattr, file, al.forkattr);
		return (EINVAL);
	}
	if (al.volattr != 0 && file != 0 && !(cmn & DA_CMN_RETURNED))	/* (a Mac takes volume attributes without ATTR_VOL_INFO too: IsPairedBootVolume asks for ATTR_VOL_CAPABILITIES alone) */
		return (EINVAL);
	if (S_ISDIR(sb.st_mode) && file != 0 && !(cmn & DA_CMN_RETURNED))
		return (EINVAL);			/* file attributes of a directory */
	name = strrchr(path, '/');
	name = (name != NULL && name[1] != '\0') ? name + 1 : path;
	namelen = strlen(name) + 1;
	out = malloc(4 + 512 + MAXPATHLEN + roundup2(namelen, 4) + 8, M_DARWIN, M_WAITOK | M_ZERO);
	p = out + 4;				/* the length comes first */
#define	PUT32(v)	do { uint32_t t_ = (uint32_t)(v); memcpy(p, &t_, 4); p += 4; } while (0)
#define	PUT64(v)	do { uint64_t t_ = (uint64_t)(v); memcpy(p, &t_, 8); p += 8; } while (0)
#define	PUTTS(ts)	do { PUT64((ts).tv_sec); PUT64((ts).tv_nsec); } while (0)
	switch (sb.st_mode & S_IFMT) {
	case S_IFREG: objtype = 1; break;
	case S_IFDIR: objtype = 2; break;
	case S_IFBLK: objtype = 3; break;
	case S_IFCHR: objtype = 4; break;
	case S_IFLNK: objtype = 5; break;
	case S_IFSOCK: objtype = 6; break;
	default: objtype = 7; break;		/* FIFO */
	}
	{
		char *nameref = NULL;

		char *pathref = NULL, *vmpref = NULL, *vnmref = NULL, *vmdref = NULL, *relref = NULL, *nfref = NULL;
		int vidx = 2;

		if (cmn & DA_CMN_RETURNED) {	/* attribute_set_t: what is in the buffer */
			PUT32(cmn); PUT32(al.volattr); PUT32(al.dirattr); PUT32(file); PUT32(al.forkattr);
		}
		if (cmn & DA_CMN_NAME) { nameref = p; p += 8; }		/* attrreference: offset (from itself), length */
		if (cmn & DA_CMN_DEVID)		PUT32(darwin_from_dev(sb.st_dev) == 16777225 && da_vol_of_path(path) >= 3 ? da_fsid[da_vol_of_path(path)] : darwin_from_dev(sb.st_dev));
		if (cmn & DA_CMN_FSID) {		/* the fsid of the volume as statfs says it (the pair must be the same, CarbonCore keys its volumes by it) */
			if (darwin_from_dev(sb.st_dev) == 16777225) { PUT32(da_vol_of_path(path) >= 3 ? da_fsid[da_vol_of_path(path)] : 16777225); PUT32(26); }	/* a Mac says the data volume (or Preboot) for the files of the system volume too; only statfs says 16777231 */
			else { PUT32(darwin_from_dev(sb.st_dev)); PUT32(0); }
		}
		if (cmn & DA_CMN_OBJTYPE)	PUT32(objtype);
		if (cmn & DA_CMN_OBJTAG)	PUT32(0);
		if (cmn & DA_CMN_OBJID)		{ PUT32(sb.st_ino); PUT32(sb.st_gen); }
		if (cmn & DA_CMN_PAROBJID)	{ PUT32(2); PUT32(0); }		/* the parent: as PARENTID, not looked up */
		if (cmn & DA_CMN_SCRIPT)	PUT32(0);			/* the text encoding: MacRoman */
		if (cmn & DA_CMN_CRTIME)	PUTTS(sb.st_mtim);		/* the files of a Mac's system were made as they are: the creation time is the modification time (a copy keeps the second, not the birth) */
		if (cmn & DA_CMN_MODTIME)	PUTTS(sb.st_mtim);
		if (cmn & DA_CMN_CHGTIME)	PUTTS(sb.st_ctim);
		if (cmn & DA_CMN_ACCTIME)	PUTTS(sb.st_atim);
		if (cmn & DA_CMN_OWNERID)	PUT32(sb.st_uid);
		if (cmn & DA_CMN_GRPID)		PUT32(sb.st_gid);
		if (cmn & DA_CMN_ACCESSMASK)	PUT32(sb.st_mode);
		if (cmn & DA_CMN_FLAGS)		PUT32(sb.st_flags);
		if (cmn & DA_CMN_GENCOUNT)	PUT32(sb.st_gen);
		if (cmn & DA_CMN_DOCID)		PUT32(0);
		if (cmn & DA_CMN_USERACCESS)	PUT32(7);
		if (cmn & DA_CMN_FILEID)	PUT64(sb.st_ino);
		if (cmn & DA_CMN_PARENTID)	PUT64(2);
		if (cmn & DA_CMN_FULLPATH)	{ pathref = p; p += 8; }
		if (cmn & DA_CMN_ADDEDTIME)	PUTTS(sb.st_mtim);
		if (cmn & DA_CMN_ERROR)		PUT32(0);
		if (cmn & DA_CMN_DATAPROT)	PUT32(0);
		if (al.volattr != 0) {
			char *mpref = NULL, *nmref = NULL, *mdref = NULL;
			int vi = da_vol_of_path(path);
			bool sysv = vi == 1;

			if (al.volattr & DA_VOL_FSTYPE)		PUT32(0x1a);			/* APFS */
			if (al.volattr & DA_VOL_SIGNATURE)	PUT32(0x4244);
			if (al.volattr & DA_VOL_SIZE)		PUT64((uint64_t)500 << 30);
			if (al.volattr & DA_VOL_SPACEFREE)	PUT64((uint64_t)100 << 30);
			if (al.volattr & DA_VOL_SPACEAVAIL)	PUT64((uint64_t)100 << 30);
			if (al.volattr & DA_VOL_MINALLOC)	PUT64(4096);
			if (al.volattr & DA_VOL_ALLOCCLUMP)	PUT64(4096);
			if (al.volattr & DA_VOL_IOBLOCKSIZE)	PUT32(4096);
			if (al.volattr & DA_VOL_OBJCOUNT)	PUT32(1200000);
			if (al.volattr & DA_VOL_FILECOUNT)	PUT32(1000000);
			if (al.volattr & DA_VOL_DIRCOUNT)	PUT32(200000);
			if (al.volattr & DA_VOL_MOUNTPOINT)	{ mpref = p; p += 8; }
			if (al.volattr & DA_VOL_NAME)		{ nmref = p; p += 8; }
			if (al.volattr & DA_VOL_MOUNTFLAGS)	PUT32(da_vols[vi].flags);
			if (al.volattr & DA_VOL_MOUNTEDDEVICE)	{ mdref = p; p += 8; }
			if (al.volattr & DA_VOL_ENCODINGSUSED)	PUT64(0x1);
			if (al.volattr & DA_VOL_CAPABILITIES) {		/* vol_capabilities_attr_t of the APFS volumes of a Mac (taken from one): capabilities[4], valid[4] */
				static const uint32_t cs[8] = { 0x039b6edf, 0x01ad6fc7, 0, 0, 0x07ffffff, 0x01ffffff, 0, 0 }, cd[8] = { 0x019b6edf, 0x01af6fc7, 0, 0, 0x07ffffff, 0x01ffffff, 0, 0 };

				memcpy(p, sysv ? cs : cd, 32); p += 32;
			}
			if (al.volattr & DA_VOL_UUID)		{ memcpy(p, da_vols[vi].uuid, 16); p += 16; }
			if (al.volattr & DA_VOL_ATTRIBUTES) {		/* vol_attributes_attr_t: the valid and the native attributes (attribute_set_t each) */
				static const uint32_t va[10] = { 0xffffffff, 0x709fffff, 0x00000007, 0x0000362f, 0x00001ffc, 0x3efffeff, 0x7086fffd, 0x00000007, 0x0000062f, 0x00001ffc };

				memcpy(p, va, 40); p += 40;
			}
			vmpref = mpref; vnmref = nmref; vmdref = mdref; vidx = vi;
		}
		if (al.dirattr & 1)		PUT32(sb.st_nlink);
		if (al.dirattr & 2)		PUT32(sb.st_nlink > 2 ? sb.st_nlink - 2 : 0);
		if (al.dirattr & 4)		PUT32(0);
		if (file & DA_FILE_LINKCOUNT)	PUT32(sb.st_nlink);
		if (file & DA_FILE_TOTALSIZE)	PUT64(sb.st_size);
		if (file & DA_FILE_ALLOCSIZE)	PUT64(sb.st_blocks * 512);
		if (file & DA_FILE_IOBLOCKSIZE)	PUT32(sb.st_blksize);
		if (file & DA_FILE_DATALENGTH)	PUT64(sb.st_size);
		if (file & DA_FILE_DATAALLOCSIZE) PUT64(sb.st_blocks * 512);
		if (al.forkattr & 0x4)		{ relref = p; p += 8; }			/* RELPATH, NOFIRMLINKPATH: attrrefs (the strings follow the fixed part) */
		if (al.forkattr & 0x8)		PUT64(sb.st_size);			/* PRIVATESIZE */
		if (al.forkattr & 0x10)		PUT64(sb.st_ino);			/* LINKID */
		if (al.forkattr & 0x20)		{ nfref = p; p += 8; }
		if (al.forkattr & 0x40)		PUT32(da_fsid[da_vol_of_path(path)]);	/* REALDEVID: the device of the volume that really holds the file (the system volume for /usr, the data volume for /Users) */
		if (al.forkattr & 0x80)		{ PUT32(da_fsid[da_vol_of_path(path)]); PUT32(26); }	/* REALFSID */
		if (al.forkattr & 0x100)	PUT64(0);				/* CLONEID */
		if (al.forkattr & 0x200)	PUT64(0);				/* EXT_FLAGS */
		if (al.forkattr & 0x400)	PUT64(0);				/* RECURSIVE_GENCOUNT */
		if (nameref != NULL) {		/* the name follows the fixed part, padded to 4 */
			int32_t off = (int32_t)(p - nameref);
			uint32_t l = (uint32_t)namelen;

			memcpy(nameref, &off, 4);
			memcpy(nameref + 4, &l, 4);
			memcpy(p, name, namelen);
			p += roundup2(namelen, 4);
		}
		if (vmpref != NULL) {
			const char *mp = da_vols[vidx].on;
			int32_t off = (int32_t)(p - vmpref); uint32_t l = (uint32_t)strlen(mp) + 1;

			memcpy(vmpref, &off, 4); memcpy(vmpref + 4, &l, 4); memcpy(p, mp, l); p += roundup2(l, 4);
		}
		if (vnmref != NULL) {
			const char *vn = da_vols[vidx].name;
			int32_t off = (int32_t)(p - vnmref); uint32_t l = (uint32_t)strlen(vn) + 1;

			memcpy(vnmref, &off, 4); memcpy(vnmref + 4, &l, 4); memcpy(p, vn, l); p += roundup2(l, 4);
		}
		if (relref != NULL || nfref != NULL) {
			/* RELPATH: the path in its volume, with the slash ("/" for the root of the volume); NOFIRMLINKPATH: where it really is (the data volume mounts under /System/Volumes/Data, a firmlink makes /Users show on "/") */
			char rbuf[MAXPATHLEN], nbuf[MAXPATHLEN + 32];
			int pvi = da_vol_of_path(path);
			const char *pre = pvi > 1 ? da_vols[pvi].on : "";
			size_t pl = strlen(pre);

			if (pvi > 1 && strncmp(path, pre, pl) == 0 && (path[pl] == '/' || path[pl] == 0)) {
				strlcpy(rbuf, path[pl] ? path + pl : "/", sizeof(rbuf));
				strlcpy(nbuf, path, sizeof(nbuf));
			} else {
				strlcpy(rbuf, path, sizeof(rbuf));
				if (pvi == 2) { strlcpy(nbuf, "/System/Volumes/Data", sizeof(nbuf)); if (strcmp(path, "/") != 0) strlcat(nbuf, path, sizeof(nbuf)); }
				else strlcpy(nbuf, path, sizeof(nbuf));
			}
			if (relref != NULL) {
				int32_t off = (int32_t)(p - relref);
				uint32_t l = (uint32_t)strlen(rbuf) + 1;

				memcpy(relref, &off, 4); memcpy(relref + 4, &l, 4); memcpy(p, rbuf, l); p += roundup2(l, 4);
			}
			if (nfref != NULL) {
				int32_t off = (int32_t)(p - nfref);
				uint32_t l = (uint32_t)strlen(nbuf) + 1;

				memcpy(nfref, &off, 4); memcpy(nfref + 4, &l, 4); memcpy(p, nbuf, l); p += roundup2(l, 4);
			}
		}
		if (vmdref != NULL) {
			const char *dv = da_vols[vidx].dev;
			int32_t off = (int32_t)(p - vmdref); uint32_t l = (uint32_t)strlen(dv) + 1;

			memcpy(vmdref, &off, 4); memcpy(vmdref + 4, &l, 4); memcpy(p, dv, l); p += roundup2(l, 4);
		}
		if (pathref != NULL) {
			int32_t off = (int32_t)(p - pathref);
			uint32_t l = (uint32_t)strlen(path) + 1;

			memcpy(pathref, &off, 4);
			memcpy(pathref + 4, &l, 4);
			memcpy(p, path, l);
			p += roundup2(l, 4);
		}
	}
	len = p - out;
	{
		uint32_t total = (uint32_t)len;

		memcpy(out, &total, 4);
	}
	attr_last_len = len;
	copy = len < bufsize ? len : bufsize;
	error = copyout(out, (void *)uap->a[2], copy);
	free(out, M_DARWIN);
	return (error);
}


/* getattrlistbulk(fd, alist, buffer, size, options): the entries of a directory, each with the attributes (the form of getattrlist, with ATTR_CMN_RETURNED_ATTRS); the count comes back, 0 at the end */
static int
darwin_getattrlistbulk(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a;
	struct file *fp;
	cap_rights_t rights;
	char dirpath[MAXPATHLEN] = "/", path[MAXPATHLEN];
	char *kbuf, *fullpath = NULL, *freepath = NULL;
	size_t bufsize = (size_t)uap->a[3], used = 0, off;
	int fd = (int)uap->a[0], error, count = 0, kb = 4096;
	off_t base = 0;
	ssize_t resid = 0;
	size_t got;
	bool full = false;
	off_t cursor, prevcur;

	if (getvnode(td, fd, cap_rights_init_one(&rights, CAP_FSTAT), &fp) != 0)
		return (EBADF);
	{
		struct vnode *vp = fp->f_vnode;

		vref(vp);
		if (vn_fullpath(vp, &fullpath, &freepath) == 0 && fullpath != NULL)
			strlcpy(dirpath, fullpath, sizeof(dirpath));
		vrele(vp);
		if (freepath != NULL)
			free(freepath, M_TEMP);
		fdrop(fp, td);
	}
	kbuf = malloc(kb, M_DARWIN, M_WAITOK);
	if (kern_lseek(td, fd, 0, SEEK_CUR) == 0) cursor = td->td_uretoff.tdu_off; else cursor = 0;	/* where the unread entries begin */
	while (!full) {
		error = kern_getdirentries(td, fd, kbuf, kb, &base, &resid, UIO_SYSSPACE);
		if (error != 0 || (got = kb - resid) == 0)
			break;
		for (off = 0; off + offsetof(struct dirent, d_name) <= got && !full; ) {
			struct dirent *de = (struct dirent *)(kbuf + off);
			struct stat sb;
			size_t elen;

			if (de->d_reclen == 0)
				break;
			off += de->d_reclen;
			prevcur = cursor;
			cursor = de->d_off;
			if (de->d_fileno == 0 || (de->d_namlen == 1 && de->d_name[0] == '.') || (de->d_namlen == 2 && de->d_name[0] == '.' && de->d_name[1] == '.'))
				continue;
			snprintf(path, sizeof(path), "%s%s%.*s", dirpath, strcmp(dirpath, "/") == 0 ? "" : "/", (int)de->d_namlen, de->d_name);
			if (kern_statat(td, AT_SYMLINK_NOFOLLOW, AT_FDCWD, path, UIO_SYSSPACE, &sb) != 0)
				continue;
			a = *uap;
			a.a[0] = 0; a.a[1] = uap->a[1]; a.a[2] = uap->a[2] + used; a.a[3] = bufsize - used; a.a[4] = uap->a[4];
			attr_last_len = 0;
			error = attr_fill(td, &a, &sb, path);
			elen = attr_last_len;
			if (error != 0)
				goto out;
			if (elen > bufsize - used) {			/* does not fit: this entry is for the next call (the position goes back to it) */
				if (count == 0) { error = ERANGE; goto out; }
				(void)kern_lseek(td, fd, prevcur, SEEK_SET);
				full = true;
				break;
			}
			used += elen;
			count++;
			if (bufsize - used < 64) { full = true; (void)kern_lseek(td, fd, cursor, SEEK_SET); break; }
		}
		if (!full && off < got)
			break;
	}
out:
	free(kbuf, M_DARWIN);
	if (error == 0)
		td->td_retval[0] = count;
	return (error);
}

static int
darwin_getattrlist(struct thread *td, struct darwin_args *uap)
{
	char path[MAXPATHLEN];
	struct stat sb;
	int error = copyinstr((void *)uap->a[0], path, sizeof(path), NULL);

	if (error != 0)
		return (error);
	error = kern_statat(td, ((uint32_t)uap->a[4] & 1) ? AT_SYMLINK_NOFOLLOW : 0, AT_FDCWD, path, UIO_SYSSPACE, &sb);
	return (error != 0 ? error : attr_fill(td, uap, &sb, path));
}

/* getattrlistat(fd, path, alist, buffer, size, options): relative to a directory */
static int
darwin_getattrlistat(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;
	char path[MAXPATHLEN];
	struct stat sb;
	int error = copyinstr((void *)uap->a[1], path, sizeof(path), NULL);

	if (error != 0)
		return (error);
	a.a[1] = uap->a[2]; a.a[2] = uap->a[3]; a.a[3] = uap->a[4]; a.a[4] = uap->a[5];
	error = kern_statat(td, ((uint32_t)uap->a[5] & 1) ? AT_SYMLINK_NOFOLLOW : 0, (int)uap->a[0] == -2 ? AT_FDCWD : (int)uap->a[0], path, UIO_SYSSPACE, &sb);
	return (error != 0 ? error : attr_fill(td, &a, &sb, path));
}

/* fgetattrlist(fd, alist, buffer, size, options): the name comes from the path of the vnode when the kernel knows one */
static int
darwin_fgetattrlist(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;
	struct stat sb;
	char fpath[MAXPATHLEN] = "?";
	struct file *fp;
	cap_rights_t rights;
	int error = kern_fstat(td, (int)uap->a[0], &sb);

	if (error == 0 && getvnode(td, (int)uap->a[0], cap_rights_init_one(&rights, CAP_FSTAT), &fp) == 0) {	/* the name and the full path come from the path of the vnode when it has one */
		char *fullpath = NULL, *freepath = NULL;
		struct vnode *vp = fp->f_vnode;

		vref(vp);
		if (vn_fullpath(vp, &fullpath, &freepath) == 0 && fullpath != NULL)
			strlcpy(fpath, fullpath, sizeof(fpath));
		vrele(vp);
		if (freepath != NULL)
			free(freepath, M_TEMP);
		fdrop(fp, td);
	}
	a.a[0] = 0; a.a[1] = uap->a[1]; a.a[2] = uap->a[2]; a.a[3] = uap->a[3]; a.a[4] = uap->a[4];
	return (error != 0 ? error : attr_fill(td, &a, &sb, fpath));
}
/* sendto to an AF_INET6 address: the family byte of the program's sockaddr is 30; it is FreeBSD's 28 for the duration of the call (and put back) */
static int
darwin_sendto6(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args a = *uap;
	uint8_t fam = 0, fix = AF_INET6;
	char *addr = (char *)uap->a[4];
	bool swapped = false;
	int error;

	a.a[3] = darwin_msgflags_pub((int)a.a[3]);
	if (addr != NULL && uap->a[5] >= 2 && copyin(addr + 1, &fam, 1) == 0 && fam == DARWIN_AF_INET6 && copyout(&fix, addr + 1, 1) == 0)
		swapped = true;
	error = sys_sendto(td, (struct sendto_args *)&a);
	if (swapped) {
		uint8_t back = DARWIN_AF_INET6;

		(void)copyout(&back, addr + 1, 1);
	}
	return (error);
}

/* ---- the entries ---- */

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	int		auevent;
	const char	*name;
} darwin_event[] = {
#define	E(num, nargs, fn, ev, name)	{ (num), (nargs), (sy_call_t *)(fn), (ev), (name) }
	E(220, 5, darwin_getattrlist,	AUE_GETATTRLIST,	"getattrlist"),
	E(228, 5, darwin_fgetattrlist,	AUE_GETATTRLIST,	"fgetattrlist"),
	E(476, 6, darwin_getattrlistat,	AUE_GETATTRLIST,	"getattrlistat"),
	E(410, 1, darwin_sigsuspend,	AUE_SIGSUSPEND,		"sigsuspend_nocancel"),
	E(14,  3, darwin_mknod,		AUE_MKNOD,		"mknod"),
	E(347, 3, darwin_getfsstat64,	AUE_GETFSSTAT,		"getfsstat64"),
	E(362, 0, sys_kqueue,		AUE_KQUEUE,		"kqueue"),
	E(363, 6, darwin_kevent,	AUE_NULL,		"kevent"),
	E(369, 7, darwin_kevent64,	AUE_NULL,		"kevent64"),
	E(374, 8, darwin_kevent_qos,	AUE_NULL,		"kevent_qos"),
	E(375, 8, darwin_kevent_id,	AUE_NULL,		"kevent_id"),
	E(461, 5, darwin_getattrlistbulk,	AUE_NULL,		"getattrlistbulk"),
	E(46,  3, darwin_sigaction,	AUE_SIGACTION,		"sigaction"),
	E(53,  2, sys_sigaltstack,	AUE_SIGALTSTACK,	"sigaltstack"),
	E(52,  1, darwin_sigpending,	AUE_SIGPENDING,		"sigpending"),
	E(111, 1, darwin_sigsuspend,	AUE_SIGSUSPEND,		"sigsuspend"),
	E(83,  3, darwin_setitimer,	AUE_SETITIMER,		"setitimer"),
	E(86,  2, darwin_getitimer,	AUE_GETITIMER,		"getitimer"),
	/* these replace the entries of syscalls_more.c (the address families) */
	E(104, 3, darwin_bind,		AUE_BIND,		"bind"),
	E(98,  3, darwin_connect,	AUE_CONNECT,		"connect"),
	E(409, 3, darwin_connect,	AUE_CONNECT,		"connect_nocancel"),
	E(30,  3, darwin_accept,	AUE_ACCEPT,		"accept"),
	E(404, 3, darwin_accept,	AUE_ACCEPT,		"accept_nocancel"),
	E(32,  3, darwin_getsockname,	AUE_GETSOCKNAME,	"getsockname"),
	E(31,  3, darwin_getpeername,	AUE_GETPEERNAME,	"getpeername"),
	E(133, 6, darwin_sendto6,	AUE_SENDTO,		"sendto"),
	E(413, 6, darwin_sendto6,	AUE_SENDTO,		"sendto_nocancel"),
	E(29,  6, darwin_recvfrom,	AUE_RECVFROM,		"recvfrom"),
	E(403, 6, darwin_recvfrom,	AUE_RECVFROM,		"recvfrom_nocancel"),
#undef E
};

void
darwin_event_syscalls_init(void)
{
	u_int i;

	for (i = 0; i < nitems(darwin_event); i++) {
		int n = darwin_event[i].num;

		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_event[i].nargs, .sy_call = darwin_event[i].fn,
		    .sy_auevent = darwin_event[i].auevent, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_event[i].name;
	}
}
