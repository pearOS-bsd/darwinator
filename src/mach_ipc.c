/*
 * Mach IPC between processes: ports are kernel objects that every task (here: every process of the Darwin kind) names with its own port names.
 *
 *   - rights: a name holds the receive right, send rights (a count), send-once rights, or nothing but a name that is dead;
 *   - messages: a header, an optional body with descriptors (port rights, out-of-line memory, out-of-line arrays of ports), data; they are copied into the
 *     queue of the port, the rights in them are held by the message until it is received;
 *   - the traps: mach_port_allocate, deallocate, mod_refs, insert_right, mach_reply_port, the self ports, mach_msg (send, receive, both, with timeouts);
 *   - the ports of a process that are special: task_self (0x203), host_self (0x1d03): messages sent to them are answered by the kernel (the MIG routines
 *     task_get_special_port, task_set_special_port, mach_ports_register and mach_ports_lookup; the others are not answered: the reply is an error);
 *   - fork: the child gets the special ports (bootstrap among them) and the registered ports of the parent; exit: the receive rights die (the senders see a dead port).
 *
 * What a launchd does (a registry of service names behind the bootstrap port) is a program of user space that holds the receive right of the bootstrap port
 * and hands it to its children; it needs nothing more than this.  Not here: the port-destroyed and send-once notifications, port attributes,
 * guards, the thread and host MIG routines (task_info, thread_create, host_info ...), voucher ports.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/capsicum.h>
#include <sys/eventhandler.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/taskqueue.h>
#include <sys/syscallsubr.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/ptrace.h>
#include <sys/vmmeter.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <bsm/audit_kevents.h>

#include "darwinator.h"

#define	KERN_SUCCESS		0
#define	KERN_INVALID_ADDRESS	1
#define	KERN_NO_SPACE		3
#define	KERN_INVALID_ARGUMENT	4
#define	KERN_FAILURE		5
#define	KERN_INVALID_NAME	15
#define	KERN_INVALID_RIGHT	17
#define	KERN_INVALID_VALUE	18

#define	MACH_PORT_RIGHT_SEND		0
#define	MACH_PORT_RIGHT_RECEIVE		1
#define	MACH_PORT_RIGHT_SEND_ONCE	2
#define	MACH_PORT_RIGHT_DEAD_NAME	4
#define	MM_MOVE_RECEIVE		16
#define	MM_MOVE_SEND		17
#define	MM_MOVE_SEND_ONCE	18
#define	MM_COPY_SEND		19
#define	MM_MAKE_SEND		20
#define	MM_MAKE_SEND_ONCE	21
#define	MACH_SEND_MSG		0x00000001
#define	MACH_RCV_MSG		0x00000002
#define	MACH_RCV_LARGE		0x00000004
#define	MACH_SEND_TIMEOUT	0x00000010
#define	MACH_RCV_TIMEOUT	0x00000100
#define	MACH_MSGH_BITS_COMPLEX	0x80000000u
#define	MACH_SEND_INVALID_DEST	0x10000003
#define	MACH_SEND_INVALID_RIGHT	0x10000007 + 0	/* replaced below */
#undef	MACH_SEND_INVALID_RIGHT
#define	MACH_SEND_INVALID_RIGHT	0x10000009
#define	MACH_SEND_INVALID_TYPE	0x1000000f
#define	MACH_SEND_INVALID_HEADER 0x10000010
#define	MACH_SEND_MSG_TOO_SMALL	0x10000008
#define	MACH_SEND_NO_BUFFER	0x1000000d
#define	MACH_SEND_INVALID_DATA	0x1000000c
#define	MACH_SEND_TOO_LARGE	0x10000007
#define	MACH_RCV_INVALID_NAME	0x10004002
#define	MACH_RCV_TIMED_OUT	0x10004003
#define	MACH_RCV_TOO_LARGE	0x10004004
#define	MACH_MSG_MAXSIZE	(1 << 20)

#define	KERN_TERMINATED		7
#define	KERN_OPERATION_TIMED_OUT 49
#define	MACH_PORT_RIGHT_PORT_SET 3
#define	NAME_TASK_SELF		0x203u
#define	NAME_HOST_SELF		0x1d03u
#define	NAME_THREAD_SELF	0x103u

MALLOC_DEFINE(M_DARWIN_IPC, "darwin_ipc", "Mach ports and messages");

enum { KP_PORT, KP_TASK, KP_HOST, KP_THREAD, KP_SET, KP_SEM, KP_FILE, KP_CLOCK, KP_BOOTSTRAP, KP_MEMENT, KP_IOKIT, KP_IOOBJ };

struct kmsg;
struct kport {
	int		refs;			/* names (entries), messages that carry it, special and registered slots */
	bool		dead;
	int		kind;
	struct proc	*kp;			/* KP_TASK: the process */
	pid_t		kpid;
	int		qn, waiters;
	TAILQ_HEAD(, kmsg) q;
	struct kport	*set;			/* the port set that this port is a member of (a port is in one set at most) */
	int		count;			/* KP_SEM: the value */
	struct kport	*dn_port;		/* a dead-name notification is sent here when this port dies (mach_port_request_notification) */
	struct file	*fp;			/* KP_FILE: a fileport holds the open file */
	struct kport	*ns_port;		/* a no-more-senders notification (id 70) is sent here when no task has a send right to this port */
	uint32_t	dn_name;		/* the name that the requester has for this port */
	struct timeout_task *timer;		/* KP_PORT made by mk_timer_create: the expiry sends a message to the port itself */
	struct vm_object *mobj;			/* KP_MEMENT: a memory entry, the pages of an object of the VM that can be mapped into other tasks (mach_make_memory_entry, mach_vm_map) */
	uint64_t	moff, msize;		/* ... where it starts in the object and how long it is */
	int		mprot;
	bool		armed;			/* the timer holds a reference on the port while armed */
	uint64_t	deadline;		/* the uptime of the expiry, in nanoseconds */
	uint64_t	context;		/* mach_port_set_context: comes back in the trailer of every message received from this port (libxpc keeps the context of a call here and reads it from the trailer) */
	struct knlist	kl;			/* the kqueue events of Darwin's EVFILT_MACHPORT: the notes of the kqueues that watch this port (or port set) */
};

struct kcarry { struct kport *port; int disp; uint32_t nameoff; };	/* a right in a message; nameoff: where its name goes in the data */
struct kool { uint8_t *data; uint32_t size; uint32_t off; int type; int disp; uint32_t count; struct kport **ports; };

struct kmsg {
	TAILQ_ENTRY(kmsg)	link;
	uint32_t		size;
	uint8_t			*data;
	int32_t			spid, suid, sgid;	/* the sender (0: the kernel), for the trailer of the receiver */
	struct kcarry		reply;		/* the local port of the header: the reply right */
	int			ncarry;
	struct kcarry		carry[8];
	int			nool;
	struct kool		ool[4];
};

struct kent { bool used, recv; int send, sonce; uint32_t name; struct kport *port; };
#define	KE_N	1024
#define	KT_N	256
struct ktask {
	bool		used;
	struct proc	*p;
	pid_t		pid;
	uint32_t	next;
	struct kent	e[KE_N];
	struct kport	*special[8];
	struct kport	*reg[3];
};
static struct ktask *kt_tab[KT_N];
static uint32_t rd32(const uint8_t *b, size_t off) { uint32_t v; memcpy(&v, b + off, 4); return (v); }
static void wr32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }
/* the bootstrap port of the system: the receive right that the task which acts as launchd sets with task_set_special_port(TASK_BOOTSTRAP_PORT); every task without one gets a send right to it (like the inheritance of the bootstrap port in a Mac).  While nobody serves it, the kernel answers as a launchd without services (KP_BOOTSTRAP). */
static struct kport *boot_global;
static pid_t launchd_pid;			/* the process of the launchd: its messages say that the sender is the process 1, which libxpc checks ("bootstrap port must lead to PID 1") */
static struct sx km_sx;
static struct mtx kl_mtx;
MTX_SYSINIT(darwin_kl, &kl_mtx, "darwin mach knotes", MTX_DEF);
SX_SYSINIT(darwin_mach, &km_sx, "darwin mach ipc");

/* ---- ports and names (the lock is held) ---- */

static struct kport *
kp_new(int kind)
{
	struct kport *p = malloc(sizeof(*p), M_DARWIN_IPC, M_WAITOK | M_ZERO);

	p->refs = 1;
	p->kind = kind;
	TAILQ_INIT(&p->q);
	knlist_init_mtx(&p->kl, &kl_mtx);
	return (p);
}

static void kmsg_free(struct kmsg *m);
static void kq_post(struct kport *p);
static void kq_post_set(struct kport *set);
static void set_leave(struct kport *p);

static void
kp_unref(struct kport *p)
{
	struct kmsg *m;

	if (p == NULL || --p->refs > 0)
		return;
	if (p->dn_port != NULL) { struct kport *n = p->dn_port; p->dn_port = NULL; kp_unref(n); }
	if (p->ns_port != NULL) { struct kport *n = p->ns_port; p->ns_port = NULL; kp_unref(n); }
	while ((m = TAILQ_FIRST(&p->q)) != NULL) {
		TAILQ_REMOVE(&p->q, m, link);
		kmsg_free(m);
	}
	if (p->fp != NULL)
		(void)fdrop(p->fp, curthread);
	if (p->timer != NULL)
		free(p->timer, M_DARWIN_IPC);
	if (p->mobj != NULL)
		vm_object_deallocate(p->mobj);
	knlist_delete(&p->kl, curthread, 0);
	knlist_destroy(&p->kl);
	free(p, M_DARWIN_IPC);
}

static void
kmsg_free(struct kmsg *m)
{
	int i, j;

	if (m->reply.port != NULL) kp_unref(m->reply.port);
	for (i = 0; i < m->ncarry; i++) kp_unref(m->carry[i].port);
	for (i = 0; i < m->nool; i++) {
		if (m->ool[i].data != NULL) free(m->ool[i].data, M_DARWIN_IPC);
		if (m->ool[i].ports != NULL) {
			for (j = 0; j < (int)m->ool[i].count; j++) kp_unref(m->ool[i].ports[j]);
			free(m->ool[i].ports, M_DARWIN_IPC);
		}
	}
	free(m->data, M_DARWIN_IPC);
	free(m, M_DARWIN_IPC);
}

static struct ktask *
kt_get(struct proc *p, bool create)
{
	int i, freei = -1;
	struct ktask *t;

	for (i = 0; i < KT_N; i++) {
		if (kt_tab[i] != NULL && kt_tab[i]->used && kt_tab[i]->p == p && kt_tab[i]->pid == p->p_pid)
			return (kt_tab[i]);
		if ((kt_tab[i] == NULL || !kt_tab[i]->used) && freei < 0)
			freei = i;
	}
	if (!create || freei < 0)
		return (NULL);
	if (kt_tab[freei] == NULL)
		kt_tab[freei] = malloc(sizeof(**kt_tab), M_DARWIN_IPC, M_WAITOK);
	t = kt_tab[freei];
	memset(t, 0, sizeof(*t));
	t->used = true; t->p = p; t->pid = p->p_pid; t->next = 0x10;
	return (t);
}

static struct kent *
ent_by_name(struct ktask *t, uint32_t name)
{
	int i;

	for (i = 0; i < KE_N; i++)
		if (t->e[i].used && t->e[i].name == name)
			return (&t->e[i]);
	return (NULL);
}

static struct kent *
ent_by_port(struct ktask *t, struct kport *p)
{
	int i;

	for (i = 0; i < KE_N; i++)
		if (t->e[i].used && t->e[i].port == p)
			return (&t->e[i]);
	return (NULL);
}

/* a name for the port in this task (the same name when the task has one already); the right is added; the entry takes a reference when it is new */
static struct kent *
ent_add(struct ktask *t, struct kport *p, int right_kind)
{
	struct kent *e = ent_by_port(t, p);
	int i;

	if (e == NULL) {
		for (i = 0; i < KE_N && t->e[i].used; i++)
			;
		if (i == KE_N)
			return (NULL);
		e = &t->e[i];
		memset(e, 0, sizeof(*e));
		e->used = true; e->port = p; p->refs++;
		e->name = (t->next++ << 8) | 3;
	}
	switch (right_kind) {
	case MM_MOVE_RECEIVE: e->recv = true; break;
	case MM_MOVE_SEND: case MM_COPY_SEND: case MM_MAKE_SEND: e->send++; break;
	case MM_MOVE_SEND_ONCE: case MM_MAKE_SEND_ONCE: e->sonce++; break;
	default: break;
	}
	return (e);
}

static void
ent_drop_if_empty(struct kent *e)
{
	if (e->used && !e->recv && e->send == 0 && e->sonce == 0) {
		kp_unref(e->port);
		e->used = false;
	}
}

/* a task with the names that every task has */
static struct ktask *
kt_task(struct proc *p)
{
	struct ktask *t = kt_get(p, false);
	struct kport *tp, *hp;
	struct kent *e;

	if (t != NULL)
		return (t);
	if ((t = kt_get(p, true)) == NULL)
		return (NULL);
	tp = kp_new(KP_TASK); tp->kp = p; tp->kpid = p->p_pid;
	hp = kp_new(KP_HOST);
	e = ent_add(t, tp, MM_MAKE_SEND); e->name = NAME_TASK_SELF; tp->refs--;	/* the entry holds the reference that kp_new gave */
	e = ent_add(t, hp, MM_MAKE_SEND); e->name = NAME_HOST_SELF; hp->refs--;
	{
		struct kport *th = kp_new(KP_THREAD);		/* thread_self: kpid 0 means the thread that asks */

		th->kp = p;
		e = ent_add(t, th, MM_MAKE_SEND); e->name = NAME_THREAD_SELF; th->refs--;
	}
	t->next = 0x20;
	return (t);
}

static void notify_dead_name(struct kport *to, uint32_t name);
static void ns_check(void);

/* the receive right of the entry is gone: the port is dead for everybody */
static void
port_die(struct kport *p)
{
	struct kmsg *m;

	p->dead = true;
	p->set = NULL;
	if (p->dn_port != NULL) {
		struct kport *np = p->dn_port;

		p->dn_port = NULL;
		notify_dead_name(np, p->dn_name);
		kp_unref(np);
	}
	wakeup(p);
	while ((m = TAILQ_FIRST(&p->q)) != NULL) {
		TAILQ_REMOVE(&p->q, m, link);
		kmsg_free(m);
	}
	p->qn = 0;
	wakeup(p);
}

/* ---- the traps of ports ---- */

static int
kr_ret(struct thread *td, int kr)
{
	td->td_retval[0] = (register_t)(uint32_t)kr;
	return (0);
}

/* ---- EVFILT_MACHPORT: the kqueue filter of Darwin that tells that a port (or a port set) has a message; libdispatch's mach sources and so XPC are built on it ----
 * FreeBSD has no free filter slot, so the slot of EVFILT_EMPTY (sockets whose send buffer is empty: used by nobody) takes it, and the kevent calls of Darwin programs name it -8.  The port is found in
 * the translation of the change (syscalls_event.c) and handed over through ext[2], checked against the table of the registrations that are on their way.  With MACH_RCV_MSG in the options the
 * event brings the message with it: the kernel receives it when the event is handed out (darwin_mach_event_receive). */
#define	EVFILT_MACHPORT_SLOT	EVFILT_EMPTY
static struct { struct kport *port; lwpid_t tid; } ev_pending[32];

static void
set_leave(struct kport *p)
{
	if (p->set != NULL && p->set->qn >= p->qn)
		p->set->qn -= p->qn;
	p->set = NULL;
}

static void
kq_post(struct kport *p)
{
	if (darwin_debug >= 2 && (p->kl.kl_list.slh_first != NULL || (p->set != NULL && p->set->kl.kl_list.slh_first != NULL)))
		printf("darwinator: kq_post port %p (qn %d)\n", p, p->qn);
	if (p->set != NULL) {
		p->set->qn++;
		KNOTE_UNLOCKED(&p->set->kl, 0);
	}
	KNOTE_UNLOCKED(&p->kl, 0);
}

static void
kq_post_set(struct kport *set)
{
	KNOTE_UNLOCKED(&set->kl, 0);
}

static int
mp_attach(struct knote *kn)
{
	struct kport *p = (struct kport *)(uintptr_t)kn->kn_kevent.ext[2];
	int i, ok = 0;

	if (curthread->td_proc->p_sysent != &darwin_sysvec)
		return (EINVAL);
	sx_xlock(&km_sx);
	for (i = 0; i < (int)nitems(ev_pending); i++)
		if (ev_pending[i].port == p && p != NULL && ev_pending[i].tid == curthread->td_tid) { ev_pending[i].port = NULL; ok = 1; break; }
	sx_xunlock(&km_sx);
	if (darwin_debug >= 1)
		printf("darwinator: %s[%d]: EVFILT_MACHPORT attach port %p kind %d: %s\n", curthread->td_proc->p_comm, curthread->td_proc->p_pid, p, p ? p->kind : -1, ok ? "ok" : "refused");
	if (!ok)
		return (EINVAL);
	kn->kn_hook = p;
	knlist_add(&p->kl, kn, 0);
	return (0);
}

static void
mp_detach(struct knote *kn)
{
	if (kn->kn_knlist != NULL)
		knlist_remove(kn->kn_knlist, kn, 0);
	kn->kn_hook = NULL;
}

static int
mp_event(struct knote *kn, long hint __unused)
{
	struct kport *p = kn->kn_hook;

	if (p == NULL || p->dead) {			/* the port is gone: the event is delivered once (EV_VANISHED) and the note goes (the program may free what udata points at: a second event would find it freed) */
		kn->kn_flags |= EV_EOF | EV_ONESHOT;
		return (1);
	}
	kn->kn_data = p->qn;
	return (p->qn > 0);
}

static const struct filterops mp_filterops = {
	.f_isfd = 0,
	.f_attach = mp_attach,
	.f_detach = mp_detach,
	.f_event = mp_event,
};

/* the port of the name in the task of the thread, for a change that adds an event: a pointer that mp_attach accepts once */
void *
darwin_mach_port_for_event(struct thread *td, uint32_t name)
{
	struct ktask *t;
	struct kent *e;
	struct kport *p = NULL;
	int i;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, name)) != NULL && e->recv) {
		for (i = 0; i < (int)nitems(ev_pending); i++)
			if (ev_pending[i].port == NULL) { ev_pending[i].port = e->port; ev_pending[i].tid = td->td_tid; p = e->port; break; }
	}
	sx_xunlock(&km_sx);
	return (p);
}


static int
darwin_port_allocate(struct thread *td, struct darwin_args *uap)	/* (target, right, *name) */
{
	struct ktask *t;
	struct kport *p;
	struct kent *e;
	uint32_t name;
	int kr = KERN_SUCCESS;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL || ((int)uap->a[1] != MACH_PORT_RIGHT_RECEIVE && (int)uap->a[1] != MACH_PORT_RIGHT_PORT_SET)) {
		sx_xunlock(&km_sx);
		return (kr_ret(td, (int)uap->a[1] == MACH_PORT_RIGHT_RECEIVE ? KERN_NO_SPACE : KERN_INVALID_VALUE));
	}
	p = kp_new((int)uap->a[1] == MACH_PORT_RIGHT_PORT_SET ? KP_SET : KP_PORT);
	e = ent_add(t, p, MM_MOVE_RECEIVE);
	p->refs--;
	if (e == NULL) { kp_unref(p); kr = KERN_NO_SPACE; }
	name = e != NULL ? e->name : 0;
	sx_xunlock(&km_sx);
	if (kr == KERN_SUCCESS && copyout(&name, (void *)uap->a[2], sizeof(name)) != 0)
		kr = KERN_INVALID_ADDRESS;
	return (kr_ret(td, kr));
}

/* _kernelrpc_mach_port_construct_trap(target, options *, context, *name): a receive right, with the options of mach_port_options_t (flags in the first word: 0x10 is MPO_INSERT_SEND_RIGHT) */
static int
darwin_port_construct(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct kport *p;
	struct kent *e;
	uint32_t flags = 0, name = 0;
	int kr = KERN_SUCCESS;

	if (uap->a[1] != 0 && copyin((void *)uap->a[1], &flags, sizeof(flags)) != 0)
		return (kr_ret(td, KERN_INVALID_ADDRESS));
	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL) {
		sx_xunlock(&km_sx);
		return (kr_ret(td, KERN_NO_SPACE));
	}
	p = kp_new(KP_PORT);
	p->context = (uint64_t)uap->a[2];		/* the context, or the guard with MPO_CONTEXT_AS_GUARD (both are ip_context in XNU): libxpc keeps the context of a call here and reads it back from the trailer of the messages */
	e = ent_add(t, p, MM_MOVE_RECEIVE);
	p->refs--;
	if (e == NULL) { kp_unref(p); kr = KERN_NO_SPACE; }
	else {
		if (flags & 0x10)
			e->send++;
		name = e->name;
	}
	sx_xunlock(&km_sx);
	if (kr == KERN_SUCCESS && copyout(&name, (void *)uap->a[3], sizeof(name)) != 0)
		kr = KERN_INVALID_ADDRESS;
	return (kr_ret(td, kr));
}

/* _kernelrpc_mach_port_destruct_trap(target, name, srdelta, guard): the receive right goes, and srdelta (negative) send rights */
static int
darwin_port_destruct(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct kent *e;
	int kr = KERN_INVALID_NAME, srdelta = (int)uap->a[2];

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		kr = KERN_SUCCESS;
		if (e->recv) { e->recv = false; port_die(e->port); }
		if (srdelta < 0 && e->send >= -srdelta) e->send += srdelta;
		ent_drop_if_empty(e);
		ns_check();
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

/* thread_get_special_reply_port (50): the special reply port of the thread (libdispatch and XPC reply on it); made on the first call, the same name after that */
static int
darwin_thread_special_reply_port(struct thread *td, struct darwin_args *uap __unused)
{
	static struct { pid_t pid; lwpid_t tid; uint32_t name; } cache[64];
	struct ktask *t;
	struct kport *p;
	struct kent *e;
	uint32_t name = 0;
	int i, slot = -1;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL) {
		for (i = 0; i < 64; i++) {
			if (cache[i].pid == td->td_proc->p_pid && cache[i].tid == td->td_tid && (e = ent_by_name(t, cache[i].name)) != NULL && e->recv) {
				name = cache[i].name;
				break;
			}
			if (slot < 0 && (cache[i].pid == 0))
				slot = i;
		}
		if (name == 0) {
			p = kp_new(KP_PORT);
			e = ent_add(t, p, MM_MOVE_RECEIVE);
			p->refs--;
			if (e != NULL) {
				name = e->name;
				if (slot < 0)
					slot = (int)(td->td_tid % 64);
				cache[slot].pid = td->td_proc->p_pid; cache[slot].tid = td->td_tid; cache[slot].name = name;
			} else
				kp_unref(p);
		}
	}
	sx_xunlock(&km_sx);
	td->td_retval[0] = name;
	return (0);
}

/* host_create_mach_voucher_trap (70)(host, recipes, size, *voucher): a voucher is a port that only has to exist (libdispatch makes them for its queues); the recipes are not looked at */
static int
darwin_host_create_voucher(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct kport *p;
	struct kent *e;
	uint32_t name = 0;
	int kr = KERN_SUCCESS;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL) {
		sx_xunlock(&km_sx);
		return (kr_ret(td, KERN_NO_SPACE));
	}
	p = kp_new(KP_PORT);
	e = ent_add(t, p, MM_MAKE_SEND);
	p->refs--;
	if (e != NULL) name = e->name; else { kp_unref(p); kr = KERN_NO_SPACE; }
	sx_xunlock(&km_sx);
	if (kr == KERN_SUCCESS && copyout(&name, (void *)uap->a[3], sizeof(name)) != 0)
		kr = KERN_INVALID_ADDRESS;
	return (kr_ret(td, kr));
}

static int
darwin_port_insert_right(struct thread *td, struct darwin_args *uap)	/* (target, name, poly, polyPoly): a send right for a port that this task names */
{
	struct ktask *t;
	struct kent *e;
	int kr = KERN_INVALID_NAME, disp = (int)uap->a[3];

	sx_xlock(&km_sx);
	t = kt_task(td->td_proc);
	if (t != NULL && (e = ent_by_name(t, (uint32_t)uap->a[2])) != NULL && (uint32_t)uap->a[1] == (uint32_t)uap->a[2]) {
		if ((disp == MM_MAKE_SEND && e->recv) || ((disp == MM_COPY_SEND || disp == MM_MOVE_SEND) && e->send > 0)) {
			if (disp != MM_MOVE_SEND) e->send++;
			kr = KERN_SUCCESS;
		} else
			kr = KERN_INVALID_RIGHT;
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
darwin_port_deallocate(struct thread *td, struct darwin_args *uap)	/* (target, name): one send or send-once right, or the dead name */
{
	struct ktask *t;
	struct kent *e;
	int kr = KERN_INVALID_NAME;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		if (e->send > 0) { e->send--; kr = KERN_SUCCESS; }
		else if (e->sonce > 0) { e->sonce--; kr = KERN_SUCCESS; }
		else if (e->recv) kr = KERN_INVALID_RIGHT;
		ent_drop_if_empty(e);
		ns_check();
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
darwin_port_mod_refs(struct thread *td, struct darwin_args *uap)	/* (target, name, right, delta) */
{
	struct ktask *t;
	struct kent *e;
	int kr = KERN_INVALID_NAME, right = (int)uap->a[2], delta = (int)uap->a[3];

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		kr = KERN_INVALID_RIGHT;
		if (right == MACH_PORT_RIGHT_RECEIVE && e->recv && delta < 0) {
			e->recv = false;
			port_die(e->port);
			kr = KERN_SUCCESS;
		} else if (right == MACH_PORT_RIGHT_SEND && delta < 0 && e->send >= -delta) {
			e->send += delta; kr = KERN_SUCCESS;
			ns_check();
		} else if (right == MACH_PORT_RIGHT_SEND && delta > 0 && e->send > 0) {
			e->send += delta; kr = KERN_SUCCESS;
		} else if (right == MACH_PORT_RIGHT_SEND_ONCE && delta < 0 && e->sonce >= -delta) {
			e->sonce += delta; kr = KERN_SUCCESS;
		}
		ent_drop_if_empty(e);
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
darwin_mach_reply_port(struct thread *td, struct darwin_args *uap __unused)
{
	struct ktask *t;
	struct kport *p;
	struct kent *e;
	uint32_t name = 0;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL) {
		p = kp_new(KP_PORT);
		e = ent_add(t, p, MM_MOVE_RECEIVE);
		p->refs--;
		if (e != NULL) name = e->name; else kp_unref(p);
	}
	sx_xunlock(&km_sx);
	td->td_retval[0] = name;
	return (0);
}

/* task_self_trap, host_self_trap, thread_self_trap: the name of the port, and (as on a Mac) one more send right to it, that the caller usually deallocates */
static int
self_port(struct thread *td, uint32_t name)
{
	struct ktask *t;
	struct kent *e;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, name)) != NULL && e->send < 4096)
		e->send++;
	sx_xunlock(&km_sx);
	td->td_retval[0] = name;
	return (0);
}
static int darwin_self_task(struct thread *td, struct darwin_args *uap __unused) { return (self_port(td, NAME_TASK_SELF)); }
static int darwin_self_host(struct thread *td, struct darwin_args *uap __unused) { return (self_port(td, NAME_HOST_SELF)); }
static int darwin_self_thread(struct thread *td, struct darwin_args *uap __unused) { return (self_port(td, NAME_THREAD_SELF)); }


static int
darwin_port_move_member(struct thread *td, struct darwin_args *uap)	/* (target, member, after): the member goes into the set "after" (0: out of its set) */
{
	struct ktask *t;
	struct kent *m, *a = NULL;
	int kr = KERN_INVALID_NAME;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (m = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		kr = KERN_INVALID_RIGHT;
		if (m->recv && m->port->kind == KP_PORT) {
			if ((uint32_t)uap->a[2] == 0) { set_leave(m->port); kr = KERN_SUCCESS; }
			else if ((a = ent_by_name(t, (uint32_t)uap->a[2])) != NULL && a->recv && a->port->kind == KP_SET) { set_leave(m->port); m->port->set = a->port; a->port->qn += m->port->qn; wakeup(a->port); if (a->port->qn > 0) kq_post_set(a->port); kr = KERN_SUCCESS; }
		}
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static uint32_t entry_type(const struct kent *e);

static int
darwin_port_extract_member(struct thread *td, struct darwin_args *uap)	/* (target, member, set) */
{
	struct ktask *t;
	struct kent *m, *a;
	int kr = KERN_INVALID_NAME;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (m = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		kr = KERN_INVALID_RIGHT;
		if ((a = ent_by_name(t, (uint32_t)uap->a[2])) != NULL && m->port->set == a->port) { set_leave(m->port); kr = KERN_SUCCESS; }
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
darwin_port_type_trap(struct thread *td, struct darwin_args *uap)	/* (target, name, *type) */
{
	struct ktask *t;
	struct kent *e;
	int kr = KERN_INVALID_NAME;
	uint32_t ty = 0;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		ty = entry_type(e);
		kr = KERN_SUCCESS;
	}
	sx_xunlock(&km_sx);
	if (kr == KERN_SUCCESS && copyout(&ty, (void *)uap->a[2], 4) != 0)
		kr = KERN_INVALID_ADDRESS;
	return (kr_ret(td, kr));
}

static int
darwin_generate_activity_id(struct thread *td, struct darwin_args *uap)	/* (target, count, *id) */
{
	static volatile uint64_t next = 1;
	uint64_t id = atomic_fetchadd_64(&next, MAX(1, (uint32_t)uap->a[1]));

	if (copyout(&id, (void *)uap->a[2], 8) != 0)
		return (kr_ret(td, KERN_INVALID_ADDRESS));
	return (kr_ret(td, KERN_SUCCESS));
}

/* ---- mk_timer: a port that receives a message when its time comes (traps 91-95) ---- */

static void
mk_timer_fire(void *arg, int pending __unused)
{
	struct kport *p = arg;
	struct kmsg *r;
	uint8_t *d;

	sx_xlock(&km_sx);
	if (p->armed) {
		p->armed = false;
		if (!p->dead) {
			r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->data = d = malloc(40, M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->size = 40;
			wr32(d, 0, MM_MOVE_SEND_ONCE);
			wr32(d, 4, 40);
			TAILQ_INSERT_TAIL(&p->q, r, link);
			p->qn++; kq_post(p);
			wakeup(p);
			if (p->set != NULL) wakeup(p->set);
		}
		kp_unref(p);
	}
	sx_xunlock(&km_sx);
}

static uint64_t
uptime_ns(void)
{
	struct timespec ts;

	nanouptime(&ts);
	return ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
}

static int
darwin_mk_timer_create(struct thread *td, struct darwin_args *uap __unused)
{
	struct ktask *t;
	struct kport *p;
	struct kent *e;
	uint32_t name = 0;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL) {
		p = kp_new(KP_PORT);
		p->timer = malloc(sizeof(*p->timer), M_DARWIN_IPC, M_WAITOK | M_ZERO);
		TIMEOUT_TASK_INIT(taskqueue_thread, p->timer, 0, mk_timer_fire, p);
		e = ent_add(t, p, MM_MOVE_RECEIVE);
		p->refs--;
		if (e == NULL) kp_unref(p); else name = e->name;
	}
	sx_xunlock(&km_sx);
	td->td_retval[0] = name;
	return (0);
}

static struct kport *
timer_of(struct thread *td, uint32_t name)
{
	struct ktask *t = kt_task(td->td_proc);
	struct kent *e = t != NULL ? ent_by_name(t, name) : NULL;

	return (e != NULL && e->recv && e->port->timer != NULL ? e->port : NULL);
}

static int
timer_stop(struct kport *p)		/* the lock is held: the time that was left, or 0 */
{
	uint64_t now = uptime_ns();
	int left = 0;

	if (p->armed) {
		left = p->deadline > now ? p->deadline - now : 0;
		if (taskqueue_cancel_timeout(taskqueue_thread, p->timer, NULL) == 0) {
			p->armed = false;
			p->refs--;
		}
	}
	return (left);
}

static int
darwin_mk_timer_destroy(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct kent *e;
	struct kport *p;
	int kr = KERN_INVALID_NAME;

	sx_xlock(&km_sx);
	if ((p = timer_of(td, (uint32_t)uap->a[0])) != NULL && (t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[0])) != NULL) {
		(void)timer_stop(p);
		e->recv = false;
		port_die(p);
		ent_drop_if_empty(e);
		ns_check();
		kr = KERN_SUCCESS;
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
timer_arm(struct thread *td, uint32_t name, uint64_t deadline)
{
	struct kport *p;
	int kr = KERN_INVALID_NAME;
	uint64_t now = uptime_ns();

	sx_xlock(&km_sx);
	if ((p = timer_of(td, name)) != NULL) {
		(void)timer_stop(p);
		p->deadline = deadline;
		p->armed = true;
		p->refs++;
		taskqueue_enqueue_timeout(taskqueue_thread, p->timer, deadline > now ? (int)MIN((deadline - now) / (1000000000 / hz), INT_MAX) : 0);
		kr = KERN_SUCCESS;
	}
	sx_xunlock(&km_sx);
	return (kr);
}

static int
darwin_mk_timer_arm(struct thread *td, struct darwin_args *uap)		/* (name, expire_time) */
{
	return (kr_ret(td, timer_arm(td, (uint32_t)uap->a[0], uap->a[1])));
}

static int
darwin_mk_timer_arm_leeway(struct thread *td, struct darwin_args *uap)	/* (name, mk_timer_flags, mk_timer_expire_time, mk_timer_leeway): the time is absolute or relative */
{
	uint64_t t = uap->a[2];

	if ((uap->a[1] & 1) == 0)
		t += uptime_ns();
	return (kr_ret(td, timer_arm(td, (uint32_t)uap->a[0], t)));
}

static int
darwin_mk_timer_cancel(struct thread *td, struct darwin_args *uap)	/* (name, *result_time) */
{
	struct kport *p;
	uint64_t left = 0;
	int kr = KERN_INVALID_NAME;

	sx_xlock(&km_sx);
	if ((p = timer_of(td, (uint32_t)uap->a[0])) != NULL) {
		left = timer_stop(p);
		kr = KERN_SUCCESS;
	}
	sx_xunlock(&km_sx);
	if (kr == KERN_SUCCESS && uap->a[1] != 0 && copyout(&left, (void *)uap->a[1], 8) != 0)
		kr = KERN_INVALID_ADDRESS;
	return (kr_ret(td, kr));
}

/* mach_port_guard / mach_port_unguard (traps 41 and 42): the guard is the context of the port (ip_context in XNU, the one that comes back in the trailer of a message); libxpc guards a port with the pointer of what it belongs to and reads it from the trailer of the answers */
static int
darwin_port_guard(struct thread *td, struct darwin_args *uap)	/* (target, name, guard[, strict]) */
{
	struct ktask *t;
	struct kent *e;
	int kr = KERN_INVALID_NAME;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		kr = KERN_SUCCESS;
		if (e->recv)
			e->port->context = (uint64_t)uap->a[2];
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
darwin_port_unguard(struct thread *td, struct darwin_args *uap)	/* (target, name, guard) */
{
	struct ktask *t;
	struct kent *e;
	int kr = KERN_INVALID_NAME;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[1])) != NULL) {
		kr = KERN_SUCCESS;
		if (e->recv && e->port->context == (uint64_t)uap->a[2])
			e->port->context = 0;
	}
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

/* ---- Mach semaphores: a port whose kind is a semaphore; created with the MIG routine of the task port, used with the traps ---- */

static struct kport *
sem_of(struct thread *td, uint32_t name, int *kr)
{
	struct ktask *t = kt_task(td->td_proc);
	struct kent *e = t != NULL ? ent_by_name(t, name) : NULL;

	if (e == NULL || e->port->kind != KP_SEM) { *kr = KERN_INVALID_NAME; return (NULL); }
	if (e->port->dead) { *kr = KERN_TERMINATED; return (NULL); }
	return (e->port);
}

static int
sem_wait_locked(struct kport *s, int timo)
{
	int error = 0;

	s->waiters++;
	while (s->count <= 0 && error == 0 && !s->dead)
		error = sx_sleep(s, &km_sx, PCATCH, "machsem", timo);
	s->waiters--;
	if (s->dead) return (KERN_TERMINATED);
	if (error == EWOULDBLOCK) return (KERN_OPERATION_TIMED_OUT);
	if (error != 0) return (14);			/* KERN_ABORTED */
	s->count--;
	return (KERN_SUCCESS);
}

static int
darwin_sem_signal(struct thread *td, struct darwin_args *uap)
{
	struct kport *s;
	int kr = KERN_SUCCESS;

	sx_xlock(&km_sx);
	if ((s = sem_of(td, (uint32_t)uap->a[0], &kr)) != NULL) { s->count++; wakeup_one(s); }
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
darwin_sem_signal_all(struct thread *td, struct darwin_args *uap)
{
	struct kport *s;
	int kr = KERN_SUCCESS;

	sx_xlock(&km_sx);
	if ((s = sem_of(td, (uint32_t)uap->a[0], &kr)) != NULL) { s->count += s->waiters; wakeup(s); }
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
darwin_sem_wait(struct thread *td, struct darwin_args *uap)
{
	struct kport *s;
	int kr = KERN_SUCCESS;

	sx_xlock(&km_sx);
	if ((s = sem_of(td, (uint32_t)uap->a[0], &kr)) != NULL)
		kr = sem_wait_locked(s, 0);
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int
sem_wait_signal(struct thread *td, uint32_t wname, uint32_t sname, int timo)
{
	struct kport *w, *g;
	int kr = KERN_SUCCESS;

	sx_xlock(&km_sx);
	if ((g = sem_of(td, sname, &kr)) != NULL) { g->count++; wakeup_one(g); }
	if (kr == KERN_SUCCESS && (w = sem_of(td, wname, &kr)) != NULL)
		kr = sem_wait_locked(w, timo);
	sx_xunlock(&km_sx);
	return (kr);
}

static int darwin_sem_wait_signal(struct thread *td, struct darwin_args *uap) { return (kr_ret(td, sem_wait_signal(td, (uint32_t)uap->a[0], (uint32_t)uap->a[1], 0))); }

static int
semtimo(uint32_t sec, int32_t nsec)
{
	int64_t t = (int64_t)sec * hz + nsec / (1000000000 / hz);

	return (t < 1 ? 1 : t > INT_MAX ? INT_MAX : (int)t);
}

static int
darwin_sem_timedwait(struct thread *td, struct darwin_args *uap)		/* (sem, sec, nsec) */
{
	struct kport *s;
	int kr = KERN_SUCCESS;

	sx_xlock(&km_sx);
	if ((s = sem_of(td, (uint32_t)uap->a[0], &kr)) != NULL)
		kr = sem_wait_locked(s, semtimo((uint32_t)uap->a[1], (int32_t)uap->a[2]));
	sx_xunlock(&km_sx);
	return (kr_ret(td, kr));
}

static int darwin_sem_timedwait_signal(struct thread *td, struct darwin_args *uap) { return (kr_ret(td, sem_wait_signal(td, (uint32_t)uap->a[0], (uint32_t)uap->a[1], semtimo((uint32_t)uap->a[2], (int32_t)uap->a[3])))); }

/* __semwait_signal(cond_sem, mutex_sem, timeout, relative, tv_sec, tv_nsec) (334, 423): what the old pthread condition variables wait with; the error is the value of the call */
int
darwin_semwait_signal(struct thread *td, struct darwin_args *uap)
{
	int timo = 0, kr;
	uint32_t cond = (uint32_t)uap->a[0], mtx = (uint32_t)uap->a[1];

	if ((int)uap->a[2] != 0) {
		int64_t sec = (int64_t)uap->a[4];
		int32_t nsec = (int32_t)uap->a[5];

		if ((int)uap->a[3] == 0) {			/* an absolute time */
			struct timespec now;

			nanotime(&now);
			sec -= now.tv_sec; nsec -= (int32_t)now.tv_nsec;
			if (nsec < 0) { sec--; nsec += 1000000000; }
			if (sec < 0) { sec = 0; nsec = 0; }
		}
		timo = semtimo((uint32_t)sec, nsec);
	}
	if (mtx != 0)
		kr = sem_wait_signal(td, cond, mtx, timo);
	else {
		struct kport *w;

		kr = KERN_SUCCESS;
		sx_xlock(&km_sx);
		if ((w = sem_of(td, cond, &kr)) != NULL)
			kr = sem_wait_locked(w, timo);
		sx_xunlock(&km_sx);
	}
	td->td_retval[0] = 0;
	return (kr == KERN_SUCCESS ? 0 : kr == 49 /* KERN_OPERATION_TIMED_OUT */ ? ETIMEDOUT : kr == 14 /* KERN_ABORTED */ ? EINTR : EINVAL);
}

/* fileport_makeport(fd, *portname) (430) and fileport_makefd(port) (431): an open file as a port, that can be sent in a message */
int
darwin_fileport_makeport(struct thread *td, struct darwin_args *uap)
{
	struct file *fp;
	struct ktask *t;
	struct kport *p;
	struct kent *e;
	uint32_t name = 0;
	int error;

	error = fget(td, (int)uap->a[0], &cap_no_rights, &fp);
	if (error != 0)
		return (error);
	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL) { sx_xunlock(&km_sx); (void)fdrop(fp, td); return (ENOMEM); }
	p = kp_new(KP_FILE);
	p->fp = fp;
	e = ent_add(t, p, MM_MAKE_SEND);
	p->refs--;
	if (e != NULL) name = e->name; else kp_unref(p);
	sx_xunlock(&km_sx);
	if (e == NULL)
		return (ENOMEM);
	return (copyout(&name, (void *)uap->a[1], sizeof(name)));
}

int
darwin_fileport_makefd(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct kent *e;
	struct file *fp = NULL;
	int fd, error;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) != NULL && (e = ent_by_name(t, (uint32_t)uap->a[0])) != NULL && e->port->kind == KP_FILE && e->port->fp != NULL) {
		fp = e->port->fp;
		if (!fhold(fp)) fp = NULL;
	}
	sx_xunlock(&km_sx);
	if (fp == NULL)
		return (EINVAL);
	error = finstall(td, fp, &fd, 0, NULL);
	(void)fdrop(fp, td);
	if (error == 0)
		td->td_retval[0] = fd;
	return (error);
}

/* ---- messages ---- */

static uint64_t rd64(const uint8_t *b, size_t off) { uint64_t v; memcpy(&v, b + off, 8); return (v); }

/* the sender gives up a right (the lock is held): the kmsg carries it; name 0 is nothing */
static int
right_out(struct ktask *t, uint32_t name, int disp, struct kcarry *c)
{
	struct kent *e;

	c->port = NULL; c->disp = disp;
	if (name == 0 || name == 0xffffffffu)
		return (0);
	if ((e = ent_by_name(t, name)) == NULL)
		return (KERN_INVALID_NAME);
	switch (disp) {
	case MM_MOVE_RECEIVE: if (!e->recv) return (KERN_INVALID_RIGHT); e->recv = false; break;
	case MM_MOVE_SEND: if (e->send < 1) return (KERN_INVALID_RIGHT); e->send--; break;
	case MM_COPY_SEND: if (e->send < 1) return (KERN_INVALID_RIGHT); break;
	case MM_MAKE_SEND: case MM_MAKE_SEND_ONCE: if (!e->recv) return (KERN_INVALID_RIGHT); break;
	case MM_MOVE_SEND_ONCE: if (e->sonce < 1) return (KERN_INVALID_RIGHT); e->sonce--; break;
	default: return (KERN_INVALID_VALUE);
	}
	c->port = e->port; e->port->refs++;
	ent_drop_if_empty(e);
	return (0);
}

/* the receiver gets a right: a name in its table; the reference of the carry moves to the entry */
static uint32_t
right_in(struct ktask *t, struct kcarry *c)
{
	struct kent *e;
	uint32_t name;

	if (c->port == NULL)
		return (0);
	if (c->port->dead && c->disp != MM_MOVE_RECEIVE)
		return (0xffffffffu);				/* MACH_PORT_DEAD */
	if ((e = ent_add(t, c->port, c->disp)) == NULL)
		return (0xffffffffu);
	name = e->name;
	kp_unref(c->port);					/* the entry has its own (a new entry took one; an old entry did not need another) */
	c->port = NULL;
	return (name);
}

static void mig_server(struct thread *td, struct ktask *t, struct kport *dest, struct kmsg *m);
static void bootstrap_server(struct thread *td, struct ktask *t, struct kport *dest, struct kmsg *m);

static int
send_msg(struct thread *td, uint64_t uaddr, uint32_t size)
{
	struct ktask *t;
	struct kmsg *m;
	struct kent *de;
	struct kport *dp;
	uint32_t bits, rdisp, ldisp, rname, lname, off, i, ndesc;
	uint8_t *d;
	int kr, nd;

	if (size < 24)
		return (MACH_SEND_MSG_TOO_SMALL);
	if (size > MACH_MSG_MAXSIZE)
		return (MACH_SEND_TOO_LARGE);
	m = malloc(sizeof(*m), M_DARWIN_IPC, M_WAITOK | M_ZERO);
	m->data = malloc(size, M_DARWIN_IPC, M_WAITOK);
	m->size = size;
	m->spid = td->td_proc->p_pid == launchd_pid ? 1 : td->td_proc->p_pid;
	m->suid = td->td_ucred->cr_uid; m->sgid = td->td_ucred->cr_gid;
	d = m->data;
	if (copyin((void *)(uintptr_t)uaddr, d, size) != 0) {
		kmsg_free(m);
		return (KERN_INVALID_ADDRESS);
	}
	bits = rd32(d, 0); rdisp = bits & 0xff; ldisp = (bits >> 8) & 0xff;
	rname = rd32(d, 8); lname = rd32(d, 12);
	if (rd32(d, 4) > size) { kmsg_free(m); return (MACH_SEND_INVALID_HEADER); }
	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL) { sx_xunlock(&km_sx); kmsg_free(m); return (KERN_NO_SPACE); }
	if ((de = ent_by_name(t, rname)) == NULL) { if (darwin_debug >= 1) printf("darwinator: %s[%d]: send to the name %#x: no such name\n", td->td_proc->p_comm, td->td_proc->p_pid, rname); sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_INVALID_DEST); }
	dp = de->port;
	if (dp->dead) { if (darwin_debug >= 1) printf("darwinator: %s[%d]: send to the name %#x: the port is dead\n", td->td_proc->p_comm, td->td_proc->p_pid, rname); sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_INVALID_DEST); }
	/* the destination right is used up (or only looked at) */
	switch (rdisp) {
	case MM_COPY_SEND: if (de->send < 1 && !de->recv) goto bad_right; break;
	case MM_MOVE_SEND: if (de->send < 1) goto bad_right; de->send--; break;
	case MM_MAKE_SEND: case MM_MAKE_SEND_ONCE: if (!de->recv) goto bad_right; break;
	case MM_MOVE_SEND_ONCE: if (de->sonce < 1) goto bad_right; de->sonce--; break;
	default: goto bad_right;
	}
	dp->refs++;
	ent_drop_if_empty(de);
	/* the reply right */
	if (lname != 0 && ldisp != 0 && (kr = right_out(t, lname, (int)ldisp, &m->reply)) != 0) {
		kp_unref(dp); sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_INVALID_RIGHT);
	}
	/* the body */
	if (bits & MACH_MSGH_BITS_COMPLEX) {
		if (size < 28) { kp_unref(dp); sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_MSG_TOO_SMALL); }
		ndesc = rd32(d, 24); off = 28;
		for (i = 0; i < ndesc; i++) {
			uint8_t type;

			if (off + 12 > size) goto bad_data;
			type = d[off + 11];
			if (type == 0) {					/* a port */
				if (m->ncarry == 8) goto bad_data;
				if ((kr = right_out(t, rd32(d, off), d[off + 10], &m->carry[m->ncarry])) != 0) { kp_unref(dp); sx_xunlock(&km_sx); m->ncarry++; kmsg_free(m); return (MACH_SEND_INVALID_RIGHT); }
				m->carry[m->ncarry].nameoff = off;
				m->ncarry++;
				off += 12;
			} else if (type == 1 || type == 3) {			/* out of line memory */
				uint64_t addr; uint32_t sz;
				struct kool *o;

				if (off + 16 > size || m->nool == 4) goto bad_data;
				addr = rd64(d, off); sz = rd32(d, off + 12);
				o = &m->ool[m->nool];
				o->type = 1; o->off = off; o->size = sz;
				if (sz > MACH_MSG_MAXSIZE) goto bad_data;
				o->data = malloc(MAX(sz, 1), M_DARWIN_IPC, M_WAITOK);
				m->nool++;
				if (sz > 0 && copyin((void *)(uintptr_t)addr, o->data, sz) != 0) goto bad_data;
				if (d[off + 8] && sz > 0) { struct munmap_args ma = { .addr = (void *)(uintptr_t)addr, .len = sz }; (void)sys_munmap(td, &ma); }
				off += 16;
			} else if (type == 2) {					/* out of line array of ports */
				uint64_t addr; uint32_t cnt, j; struct kool *o; uint32_t *names;

				if (off + 16 > size || m->nool == 4) goto bad_data;
				addr = rd64(d, off); cnt = rd32(d, off + 12);
				if (cnt > 64) goto bad_data;
				o = &m->ool[m->nool];
				o->type = 2; o->off = off; o->disp = d[off + 10]; o->count = 0;
				o->ports = malloc(MAX(cnt, 1) * sizeof(*o->ports), M_DARWIN_IPC, M_WAITOK | M_ZERO);
				m->nool++;
				names = malloc(MAX(cnt, 1) * 4, M_DARWIN_IPC, M_WAITOK);
				if (cnt > 0 && copyin((void *)(uintptr_t)addr, names, cnt * 4) != 0) { free(names, M_DARWIN_IPC); goto bad_data; }
				for (j = 0; j < cnt; j++) {
					struct kcarry c;

					if (right_out(t, names[j], o->disp, &c) != 0) { free(names, M_DARWIN_IPC); goto bad_data; }
					o->ports[j] = c.port;			/* NULL for the null name */
					o->count = j + 1;
				}
				free(names, M_DARWIN_IPC);
				off += 16;
			} else
				goto bad_data;
		}
	}
	nd = dp->kind;
	if (darwin_debug >= 2) {
		int ci;

		for (ci = 0; ci < m->ncarry; ci++)
			printf("darwinator: %s[%d]: message id %#x carries port %p kind %d (disposition %d)\n", td->td_proc->p_comm, td->td_proc->p_pid, rd32(d, 20), m->carry[ci].port, m->carry[ci].port ? m->carry[ci].port->kind : -1, m->carry[ci].disp);
	}
	if (darwin_debug >= 2)
		printf("darwinator: %s[%d]: message id %#x to name %#x (port %p kind %d, %d queued)\n", td->td_proc->p_comm, td->td_proc->p_pid, rd32(d, 20), rname, dp, nd, dp->qn);
	if (nd == KP_BOOTSTRAP) {					/* the bootstrap port: the kernel answers as a launchd that has no services */
		bootstrap_server(td, t, dp, m);
		kp_unref(dp);
		sx_xunlock(&km_sx);
		kmsg_free(m);
		return (KERN_SUCCESS);
	}
	if (nd != KP_PORT) {						/* the kernel answers for the task and host ports */
		mig_server(td, t, dp, m);
		kp_unref(dp);
		sx_xunlock(&km_sx);
		kmsg_free(m);
		return (KERN_SUCCESS);
	}
	if (dp->qn >= 64) { kp_unref(dp); sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_NO_BUFFER); }
	TAILQ_INSERT_TAIL(&dp->q, m, link);
	dp->qn++; kq_post(dp);
	wakeup(dp);
	if (dp->set != NULL)
		wakeup(dp->set);
	kp_unref(dp);						/* the queue does not hold the port: the receive right does */
	sx_xunlock(&km_sx);
	return (KERN_SUCCESS);
bad_right:
	sx_xunlock(&km_sx);
	kmsg_free(m);
	return (MACH_SEND_INVALID_RIGHT);
bad_data:
	kp_unref(dp);
	sx_xunlock(&km_sx);
	kmsg_free(m);
	return (MACH_SEND_INVALID_DATA);
}

static int
user_alloc(struct thread *td, size_t len, uint64_t *addr)
{
	struct mmap_args ma = { .addr = NULL, .len = len, .prot = PROT_READ | PROT_WRITE, .flags = MAP_ANON | MAP_PRIVATE, .fd = -1, .pos = 0 };
	int error = sys_mmap(td, &ma);

	if (error == 0)
		*addr = (uint64_t)td->td_retval[0];
	return (error);
}

/* the size of the trailer that the options of a receive ask for: the type and size, the sequence number, the sender, the audit token, the context */
static uint32_t
trailer_size(uint32_t option)
{
	uint32_t el = (option >> 24) & 0xf;

	return (el == 0 ? 8 : el == 1 ? 12 : el == 2 ? 20 : el == 3 ? 52 : el == 4 ? 60 : el == 5 || el == 7 ? 64 : 68);	/* 4 context, 5 and 7 the filter policy id (msgh_ad), the others the labels too */
}

static int
recv_msg_sz(struct thread *td, uint64_t uaddr, uint32_t option, uint32_t rsize, uint32_t rname, uint32_t timeout_ms, uint32_t *msgsz)
{
	struct ktask *t;
	struct kent *e;
	struct kport *rp, *srcp = NULL;
	uint32_t setname = rname;
	struct kmsg *m;
	int timo = (option & MACH_RCV_TIMEOUT) ? MAX(1, (int)((int64_t)timeout_ms * hz / 1000)) : 0, error = 0, i;
	uint8_t *d;
	uint32_t bits, ldisp0;
	uint64_t ctxv = 0;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL || (e = ent_by_name(t, rname)) == NULL || !e->recv) {
		sx_xunlock(&km_sx);
		return (MACH_RCV_INVALID_NAME);
	}
	rp = e->port; rp->refs++;
	rp->waiters++;
	for (;;) {
		struct kport *src = rp;

		if (rp->kind == KP_SET) {			/* a set: the first member that has a message */
			int k;

			src = NULL;
			for (k = 0; k < KE_N; k++)
				if (t->e[k].used && t->e[k].port->set == rp && !TAILQ_EMPTY(&t->e[k].port->q)) { src = t->e[k].port; rname = t->e[k].name; break; }
		}
		if (src != NULL && (m = TAILQ_FIRST(&src->q)) != NULL) { srcp = src; break; }
		m = NULL;
		if (error != 0 || rp->dead)
			break;
		error = sx_sleep(rp, &km_sx, PCATCH, "machmsg", timo);
		if ((e = ent_by_name(t, setname)) == NULL || !e->recv || e->port != rp)
			error = EINVAL;
	}
	rp->waiters--;
	if (m == NULL) {
		kp_unref(rp);
		sx_xunlock(&km_sx);
		return (error == EWOULDBLOCK ? MACH_RCV_TIMED_OUT : error == EINTR || error == ERESTART ? 0x10004005 /* MACH_RCV_INTERRUPTED */ : MACH_RCV_INVALID_NAME);
	}
	if (((m->size + 3) & ~3u) + trailer_size(option) > rsize) {		/* the message and its trailer have to fit, as in XNU (the receiver tries again with a bigger buffer when it asked for MACH_RCV_LARGE) */
		if (msgsz != NULL)
			*msgsz = ((m->size + 3) & ~3u) + trailer_size(option);
		if ((option & 0x4) != 0 && uaddr != 0) {		/* MACH_RCV_LARGE: the size of the message goes into the header of the buffer (msgh_size), so that the receiver can try again with a buffer that fits */
			uint32_t need = m->size;

			(void)copyout(&need, (void *)(uaddr + 4), sizeof(need));
		}
		kp_unref(rp);
		sx_xunlock(&km_sx);
		return (MACH_RCV_TOO_LARGE);
	}
	TAILQ_REMOVE(&srcp->q, m, link);
	ctxv = srcp->context;
	srcp->qn--;
	if (srcp->set != NULL && srcp->set->qn > 0)
		srcp->set->qn--;
	d = m->data;
	/* the header as the receiver sees it: the reply right as the remote port, the destination as the local port */
	bits = rd32(d, 0);
	ldisp0 = (bits >> 8) & 0xff;
	{
		uint32_t rdisp0 = bits & 0xff;
		uint32_t reply = right_in(t, &m->reply);
		uint32_t newr = ldisp0 == MM_MAKE_SEND_ONCE || ldisp0 == MM_MOVE_SEND_ONCE ? MM_MOVE_SEND_ONCE : MM_MOVE_SEND;
		uint32_t newl = rdisp0 == MM_MAKE_SEND_ONCE || rdisp0 == MM_MOVE_SEND_ONCE ? MM_MOVE_SEND_ONCE : MM_MOVE_SEND;

		wr32(d, 0, (reply != 0 ? newr : 0) | (newl << 8) | (bits & MACH_MSGH_BITS_COMPLEX));
		wr32(d, 8, reply);
		wr32(d, 12, rname);
	}
	for (i = 0; i < m->ncarry; i++) {
		int disp = m->carry[i].disp;
		uint32_t off = m->carry[i].nameoff, name;

		name = right_in(t, &m->carry[i]);
		if (darwin_debug >= 2)
			printf("darwinator: %s[%d]: received a port %p with disposition %d as the name %#x\n", td->td_proc->p_comm, td->td_proc->p_pid, m->carry[i].port, disp, name);
		wr32(d, off, name);
		d[off + 10] = disp == MM_MOVE_RECEIVE ? 16 : (disp == MM_MOVE_SEND_ONCE || disp == MM_MAKE_SEND_ONCE) ? 18 : 17;
	}
	m->ncarry = 0;
	kp_unref(rp);
	sx_xunlock(&km_sx);
	for (i = 0; i < m->nool; i++) {		/* out of line data: memory in the receiver */
		struct kool *o = &m->ool[i];
		uint64_t addr = 0;
		size_t len = o->type == 1 ? o->size : (size_t)o->count * 4;

		if (len > 0 && user_alloc(td, round_page(len), &addr) == 0) {
			if (o->type == 1)
				(void)copyout(o->data, (void *)(uintptr_t)addr, len);
			else {
				uint32_t j, *names = malloc(MAX(o->count, 1) * 4, M_DARWIN_IPC, M_WAITOK | M_ZERO);
				struct kcarry c;

				sx_xlock(&km_sx);
				for (j = 0; j < o->count; j++) { c.port = o->ports[j]; c.disp = o->disp; names[j] = right_in(t, &c); o->ports[j] = NULL; }
				sx_xunlock(&km_sx);
				(void)copyout(names, (void *)(uintptr_t)addr, o->count * 4);
				free(names, M_DARWIN_IPC);
			}
		}
		memcpy(d + o->off, &addr, 8);
		d[o->off + 8] = 0;			/* deallocate: the receiver owns the memory */
		if (o->type == 2) d[o->off + 10] = o->disp == MM_MOVE_SEND_ONCE || o->disp == MM_MAKE_SEND_ONCE ? 18 : 17;
	}
	error = copyout(d, (void *)(uintptr_t)uaddr, m->size);
	if (error == 0) {		/* the trailer: the elements that the receiver asked for (the type and the size; the sequence number; the sender; the audit token), as far as the buffer goes */
		uint32_t tsize = trailer_size(option), at = (m->size + 3) & ~3u, tr[17] = { 0 };

		if (msgsz != NULL)
			*msgsz = at + tsize;
		tr[0] = 0; tr[1] = tsize; tr[2] = 0;
		tr[3] = m->suid; tr[4] = m->sgid;				/* the security token */
		tr[5] = 0xffffffffu; tr[6] = m->suid; tr[7] = m->sgid; tr[8] = m->suid; tr[9] = m->sgid;	/* auid euid egid ruid rgid, as TASK_AUDIT_TOKEN gives them */
		tr[10] = m->spid; tr[11] = m->spid; tr[12] = 1;		/* pid asid pidversion (the exec count: 1, as in the token of the task) */
		tr[13] = (uint32_t)ctxv; tr[14] = (uint32_t)(ctxv >> 32);	/* the context of the port (the trailer of 60 bytes) */
		if (darwin_debug >= 2)
			printf("darwinator: %s[%d]: received id %#x, size %u, option %#x, buffer %u: trailer %u bytes (sender pid %d)%s\n", td->td_proc->p_comm, td->td_proc->p_pid, rd32(d, 20), m->size, option, rsize, tsize, m->spid, at + tsize <= rsize ? "" : " DOES NOT FIT");
		if (at + tsize <= rsize)
			(void)copyout(tr, (void *)(uintptr_t)(uaddr + at), tsize);
	}
	sx_xlock(&km_sx);
	for (i = 0; i < m->nool; i++) {			/* the memory was copied out; the ports of the arrays were handed out (o->ports[j] = NULL above), their array and the data go */
		if (m->ool[i].data != NULL) { free(m->ool[i].data, M_DARWIN_IPC); m->ool[i].data = NULL; }
		if (m->ool[i].ports != NULL) { free(m->ool[i].ports, M_DARWIN_IPC); m->ool[i].ports = NULL; }
	}
	m->nool = 0;
	kmsg_free(m);
	sx_xunlock(&km_sx);
	return (error != 0 ? KERN_INVALID_ADDRESS : KERN_SUCCESS);
}

static int
recv_msg(struct thread *td, uint64_t uaddr, uint32_t option, uint32_t rsize, uint32_t rname, uint32_t timeout_ms)
{
	return (recv_msg_sz(td, uaddr, option, rsize, rname, timeout_ms, NULL));
}

static int darwin_mach_msg(struct thread *td, struct darwin_args *uap);

/* the receive that goes with an event of EVFILT_MACHPORT (with MACH_RCV_MSG in the options): the message of the port into the buffer, without waiting */
int
darwin_mach_event_receive(struct thread *td, uint32_t name, uint32_t option, uint64_t buf, uint32_t bufsize, uint32_t *msgsize)
{
	if (darwin_debug >= 2)
		printf("darwinator: %s[%d]: event receive on %#x, option %#x, buffer %#lx (%u)\n", td->td_proc->p_comm, td->td_proc->p_pid, name, option, (u_long)buf, bufsize);
	return ((int)recv_msg_sz(td, buf, (option & ~0x100u) | MACH_RCV_TIMEOUT, bufsize, name, 0, msgsize));
}

/*
 * mach_msg2_trap (47), the trap that libsyscall uses since macOS 12: (data, options, bits | send_size << 32, remote | local << 32, voucher | id << 32,
 * descriptor count | rcv_name << 32, rcv_size | priority << 32, timeout) (checked with the requests of dyld: bits 0x1513, send_size 40, id 200).  The header comes in registers and the message is in memory at data; here the header is written
 * to the memory (it is the same values) and the classic path does the rest.  The vector form (options bit 32: data points to two vectors) uses the first vector.
 */
static int
darwin_mach_msg2(struct thread *td, struct darwin_args *uap)
{
	struct darwin_args ua;
	uint64_t data = uap->a[0], opt = uap->a[1], rcv_addr = 0;
	uint32_t send_size = (uint32_t)(uap->a[2] >> 32), rcv_size = (uint32_t)uap->a[6];
	uint32_t hdr[6];
	int error;

	if (opt & (1ULL << 32)) {			/* MACH64_MSG_VECTOR: { data, rcv_addr, send_size, rcv_size } x 2 */
		uint64_t vec[3];

		error = copyin((void *)data, vec, sizeof(vec));
		if (error != 0)
			return (kr_ret(td, KERN_INVALID_ADDRESS));
		data = vec[0]; rcv_addr = vec[1];
		send_size = (uint32_t)vec[2]; rcv_size = (uint32_t)(vec[2] >> 32);
		(void)rcv_addr;
	}
	if (opt & 1) {					/* MACH_SEND_MSG: the header is in the registers */
		hdr[0] = (uint32_t)uap->a[2]; hdr[1] = send_size;
		hdr[2] = (uint32_t)uap->a[3]; hdr[3] = (uint32_t)(uap->a[3] >> 32);
		hdr[4] = (uint32_t)uap->a[4]; hdr[5] = (uint32_t)(uap->a[4] >> 32);
		if (send_size >= sizeof(hdr) && copyout(hdr, (void *)data, sizeof(hdr)) != 0)
			return (kr_ret(td, KERN_INVALID_ADDRESS));
	}
	memset(&ua, 0, sizeof(ua));
	ua.a[0] = data;
	ua.a[1] = (uint32_t)opt;
	ua.a[2] = send_size;
	ua.a[3] = rcv_size;
	ua.a[4] = (uint32_t)(uap->a[5] >> 32);		/* the receive name */
	ua.a[5] = (uint32_t)uap->a[7];			/* the timeout (ms) */
	ua.a[6] = 0;
	error = darwin_mach_msg(td, &ua);
	if (darwin_debug >= 1 && td->td_retval[0] != 0 && (opt & 1) != 0)
		printf("darwinator: %s[%d]: mach_msg send failed: kr %#lx, options %#lx, remote name %#x, id %#x, send_size %u, rcv_size %u\n", td->td_proc->p_comm, td->td_proc->p_pid, (u_long)td->td_retval[0], (u_long)opt, hdr[3], hdr[5], send_size, rcv_size);
	if (darwin_debug >= 2)
		printf("darwinator: mach_msg2: options %#lx send_size %u rcv_size %u rcv_name %#x timeout %lu bits %#lx remote_local %#lx voucher_id %#lx -> kr %#lx\n", (u_long)opt, send_size, rcv_size,
		    (uint32_t)(uap->a[5] >> 32), (u_long)uap->a[7], (u_long)(uap->a[2] & 0xffffffff), (u_long)uap->a[3], (u_long)uap->a[4], (u_long)td->td_retval[0]);
	if (darwin_debug >= 2 && (opt & 2) && td->td_retval[0] == 0) {		/* the reply, as it is in the memory of the program */
		uint32_t w[26];

		memset(w, 0, sizeof(w));
		if (copyin((void *)data, w, sizeof(w)) == 0)
			printf("darwinator: reply: %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15], w[16]);
	}
	return (error);
}

static int
darwin_mach_msg(struct thread *td, struct darwin_args *uap)	/* (msg, option, send_size, rcv_size, rcv_name, timeout, notify) */
{
	uint32_t option = (uint32_t)uap->a[1], kr = KERN_SUCCESS;

	if (option & MACH_SEND_MSG)
		kr = (uint32_t)send_msg(td, uap->a[0], (uint32_t)uap->a[2]);
	if (darwin_debug >= 2 && kr != KERN_SUCCESS)
		printf("darwinator: mach_msg: the send failed: %#x\n", kr);
	if (kr == KERN_SUCCESS && (option & MACH_RCV_MSG))
		kr = (uint32_t)recv_msg(td, uap->a[0], option, (uint32_t)uap->a[3], (uint32_t)uap->a[4], (uint32_t)uap->a[5]);
	if (darwin_debug >= 2 && kr != KERN_SUCCESS)
		printf("darwinator: mach_msg: failed: %#x\n", kr);
	td->td_retval[0] = kr;
	return (0);
}

/* ---- the MIG routines of the task port ---- */

/* a reply into the reply port of the request (the lock is held): err != 0 is a mig_reply_error_t, else the caller gives the body */
static void
mig_reply(struct ktask *t, struct kmsg *req, int id, int kr, struct kport *give, uint8_t *body, size_t bodylen, struct kport **oolports, uint32_t oolcount)
{
	struct kmsg *r;
	struct kport *rp;
	size_t size;
	uint8_t *d;

	if (req->reply.port == NULL || req->reply.port->dead)
		return;
	rp = req->reply.port;
	size = give != NULL ? 40 : oolports != NULL ? 56 : 36 + bodylen;
	if (kr != 0) { give = NULL; oolports = NULL; size = 36; }
	r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->data = d = malloc(size, M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->size = size;
	wr32(d, 4, size);
	wr32(d, 20, id + 100);			/* the reply has the id of the request plus 100 */
	if (give != NULL) {
		wr32(d, 0, MACH_MSGH_BITS_COMPLEX | (MM_MOVE_SEND_ONCE));
		wr32(d, 24, 1);
		r->carry[0].port = give; give->refs++; r->carry[0].disp = MM_MOVE_SEND; r->carry[0].nameoff = 28;
		r->ncarry = 1;
		d[28 + 10] = MM_MOVE_SEND; d[28 + 11] = 0;
	} else if (oolports != NULL) {
		struct kool *o = &r->ool[0];
		uint32_t j;

		wr32(d, 0, MACH_MSGH_BITS_COMPLEX | (MM_MOVE_SEND_ONCE));
		wr32(d, 24, 1);
		o->type = 2; o->off = 28; o->disp = MM_MOVE_SEND; o->count = oolcount;
		o->ports = malloc(MAX(oolcount, 1) * sizeof(*o->ports), M_DARWIN_IPC, M_WAITOK | M_ZERO);
		for (j = 0; j < oolcount; j++) { o->ports[j] = oolports[j]; if (oolports[j] != NULL) oolports[j]->refs++; }
		r->nool = 1;
		d[28 + 11] = 2; wr32(d, 28 + 12, oolcount);
		wr32(d, 44 + 4, 0); wr32(d, size - 4, oolcount);
	} else {
		wr32(d, 0, MM_MOVE_SEND_ONCE);
		wr32(d, size - 4 - bodylen, (uint32_t)kr);
		if (bodylen > 0) memcpy(d + size - bodylen, body, bodylen);
	}
	if (give == NULL && oolports == NULL)
		wr32(d, 32, (uint32_t)kr);
	TAILQ_INSERT_TAIL(&rp->q, r, link);
	rp->qn++; kq_post(rp);
	wakeup(rp);
	(void)t;
}


/* MACH_NOTIFY_DEAD_NAME (72): header, NDR, the name (mach_dead_name_notification_t) */
static void
notify_id(struct kport *to, int nid, uint32_t arg)
{
	struct kmsg *r;
	uint8_t *d;

	if (to->dead)
		return;
	r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->data = d = malloc(36, M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->size = 36;
	wr32(d, 0, MM_MOVE_SEND_ONCE);
	wr32(d, 4, 36);
	wr32(d, 20, nid);
	wr32(d, 32, arg);
	TAILQ_INSERT_TAIL(&to->q, r, link);
	to->qn++; kq_post(to);
	wakeup(to);
}

static void
notify_dead_name(struct kport *to, uint32_t name)
{
	notify_id(to, 72, name);
}

/* the ports that have a no-more-senders request (each holds a reference); checked whenever a send right goes away */
#define	NS_N	32
static struct kport *ns_list[NS_N];

static int
ns_sends(struct kport *p)
{
	int i, j, n = 0;

	for (i = 0; i < KT_N; i++) {
		if (kt_tab[i] == NULL || !kt_tab[i]->used)
			continue;
		for (j = 0; j < KE_N; j++)
			if (kt_tab[i]->e[j].used && kt_tab[i]->e[j].port == p)
				n += kt_tab[i]->e[j].send;
	}
	return (n);
}

static void
ns_check(void)
{
	int i;

	for (i = 0; i < NS_N; i++) {
		struct kport *p = ns_list[i];

		if (p == NULL)
			continue;
		if (p->dead || p->ns_port == NULL) {
			ns_list[i] = NULL;
			kp_unref(p);
		} else if (ns_sends(p) == 0) {
			struct kport *np = p->ns_port;

			p->ns_port = NULL;
			notify_id(np, 70, 0);			/* MACH_NOTIFY_NO_SENDERS: the make-send count follows the NDR */
			kp_unref(np);
			ns_list[i] = NULL;
			kp_unref(p);
		}
	}
}

/* a reply with the return code and inline data after it (the layout of MIG for simple routines: header 24, NDR 8, RetCode 4, data) */
static void
mig_reply_data(struct kmsg *req, int id, int kr, const void *data, size_t datalen)
{
	struct kmsg *r;
	struct kport *rp;
	size_t size = 36 + (kr == 0 ? roundup2(datalen, 4) : 0);
	uint8_t *d;

	if (req->reply.port == NULL || req->reply.port->dead)
		return;
	rp = req->reply.port;
	r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->data = d = malloc(size, M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->size = size;
	wr32(d, 0, MM_MOVE_SEND_ONCE);
	wr32(d, 4, size);
	wr32(d, 20, id + 100);
	wr32(d, 32, (uint32_t)kr);
	if (kr == 0 && datalen > 0)
		memcpy(d + 36, data, datalen);
	TAILQ_INSERT_TAIL(&rp->q, r, link);
	rp->qn++; kq_post(rp);
	wakeup(rp);
}

static uint32_t
entry_type(const struct kent *e)
{
	uint32_t type = 0;

	if (e->recv) type |= 1 << 17;				/* MACH_PORT_TYPE_RECEIVE */
	if (e->send > 0) type |= 1 << 16;			/* MACH_PORT_TYPE_SEND */
	if (e->sonce > 0) type |= 1 << 18;			/* MACH_PORT_TYPE_SEND_ONCE */
	if (e->port->dead && !e->recv) type |= 1 << 20;		/* MACH_PORT_TYPE_DEAD_NAME */
	if (e->port->kind == KP_SET) type = 1 << 19;		/* MACH_PORT_TYPE_PORT_SET */
	return (type);
}

/* the routines that only answer questions: the answers are what this system can say */
static bool
mig_info(struct thread *td, struct ktask *t, struct kport *dest, struct kmsg *m, int id)
{
	uint8_t *d = m->data;
	struct proc *p = dest->kind == KP_TASK && dest->kp != NULL ? dest->kp : td->td_proc;
	uint32_t a, b, out[64];
	int n;

	memset(out, 0, sizeof(out));
	if (darwin_debug >= 2)
		printf("darwinator: %s[%d]: MIG request %d to a port of kind %d\n", td->td_proc->p_comm, td->td_proc->p_pid, id, dest->kind);
	switch (id) {
	case 3201:					/* mach_port_type(task, name, *ptype) */
		{
			struct kent *e = m->size >= 36 ? ent_by_name(t, rd32(d, 32)) : NULL;
			uint32_t type;

			if (e == NULL) { mig_reply_data(m, id, KERN_INVALID_NAME, NULL, 0); return (true); }
			type = entry_type(e);
			mig_reply_data(m, id, 0, &type, 4);
			return (true);
		}
	case 3215:					/* mach_port_extract_right(task, name, msgt_name, *poly, *poly_type): a right to the port of the name, in the reply (a send-once or a send right made from the receive right) */
		{
			struct kent *e = m->size >= 40 ? ent_by_name(t, rd32(d, 32)) : NULL;
			uint32_t mt = m->size >= 40 ? rd32(d, 36) : 0;
			struct kmsg *r;
			uint8_t *rd;
			int disp = mt == MM_MAKE_SEND_ONCE || mt == MM_MOVE_SEND_ONCE ? MM_MAKE_SEND_ONCE : MM_MAKE_SEND;

			if (e == NULL || !e->recv) { mig_reply_data(m, id, e == NULL ? KERN_INVALID_NAME : KERN_INVALID_RIGHT, NULL, 0); return (true); }
			if (m->reply.port == NULL || m->reply.port->dead)
				return (true);
			r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->data = rd = malloc(40, M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->size = 40;							/* only the port descriptor: the type of the right is its disposition (no NDR, no other field) */
			wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
			wr32(rd, 4, 40); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
			r->carry[0].port = e->port; e->port->refs++; r->carry[0].disp = disp; r->carry[0].nameoff = 28;
			r->ncarry = 1;
			rd[28 + 10] = disp == MM_MAKE_SEND_ONCE ? 18 : 17; rd[28 + 11] = 0;
			TAILQ_INSERT_TAIL(&m->reply.port->q, r, link);
			m->reply.port->qn++; kq_post(m->reply.port);
			wakeup(m->reply.port);
			return (true);
		}
	case 3217:					/* mach_port_get_attributes(task, name, flavor, *info, *count) */
		{
			struct kent *e = m->size >= 44 ? ent_by_name(t, rd32(d, 32)) : NULL;

			a = m->size >= 44 ? rd32(d, 36) : 0;
			if (e == NULL) { mig_reply_data(m, id, KERN_INVALID_NAME, NULL, 0); return (true); }
			if (a == 1) { out[0] = 1; out[1] = 5; n = 1; }				/* MACH_PORT_LIMITS_INFO: the queue limit (5) */
			else if (a == 2) {							/* MACH_PORT_RECEIVE_STATUS: mach_port_status_t, 10 ints */
				out[0] = 1; out[1] = 0 /* mps_pset */; out[2] = 0 /* seqno */; out[3] = 0 /* mscount */; out[4] = 5 /* qlimit */;
				out[5] = (uint32_t)e->port->qn /* msgcount */; out[6] = 0; out[7] = 0; out[8] = 0; out[9] = 0; n = 10;
				out[0] = 0; out[1] = e->port->set != NULL ? 1 : 0;
			} else if (a == 3) { out[0] = 0; n = 1; }				/* MACH_PORT_DNREQUESTS_SIZE */
			else { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			{
				uint32_t buf[2 + 16];

				buf[0] = (uint32_t)n;
				memcpy(buf + 1, out, n * 4);
				mig_reply_data(m, id, 0, buf, (n + 1) * 4);			/* infoCnt, then the info */
			}
			return (true);
		}
	case 3612:					/* thread_info(thread, flavor, *info, *count): THREAD_BASIC_INFO (3), THREAD_IDENTIFIER_INFO (4) */
		{
			uint32_t buf[1 + 12];
			uint64_t tid = dest->kpid != 0 ? (uint64_t)dest->kpid : (uint64_t)td->td_tid;

			if (dest->kind != KP_THREAD) return (false);
			a = m->size >= 40 ? rd32(d, 32) : 0;
			memset(buf, 0, sizeof(buf));
			if (a == 3) { buf[0] = 10; buf[6] = 1 /* POLICY_TIMESHARE */; buf[7] = 1 /* TH_STATE_RUNNING */; }
			else if (a == 4) { buf[0] = 6; memcpy(&buf[1], &tid, 8); }
			else { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			mig_reply_data(m, id, 0, buf, (buf[0] + 1) * 4);
			return (true);
		}
	case 3603:					/* thread_get_state(thread, flavor, *state, *count): x86_THREAD_STATE64 (4) of the calling thread */
		{
			if (dest->kind != KP_THREAD) return (false);
#if defined(__amd64__)
			a = m->size >= 40 ? rd32(d, 32) : 0;
			if (a == 4 && dest->kpid == 0) {
				uint64_t r[21];
				struct trapframe *tf = td->td_frame;

				r[0] = tf->tf_rax; r[1] = tf->tf_rbx; r[2] = tf->tf_rcx; r[3] = tf->tf_rdx; r[4] = tf->tf_rdi; r[5] = tf->tf_rsi;
				r[6] = tf->tf_rbp; r[7] = tf->tf_rsp; r[8] = tf->tf_r8; r[9] = tf->tf_r9; r[10] = tf->tf_r10; r[11] = tf->tf_r11;
				r[12] = tf->tf_r12; r[13] = tf->tf_r13; r[14] = tf->tf_r14; r[15] = tf->tf_r15; r[16] = tf->tf_rip; r[17] = tf->tf_rflags;
				r[18] = tf->tf_cs; r[19] = 0; r[20] = 0;
				{
					uint32_t buf[1 + 42];

					buf[0] = 42;
					memcpy(buf + 1, r, sizeof(r));
					mig_reply_data(m, id, 0, buf, sizeof(buf));
				}
				return (true);
			}
#endif
			mig_reply_data(m, id, KERN_FAILURE, NULL, 0);
			return (true);
		}
	case 3605: case 3606: case 3607: case 3608: case 3609:		/* thread_suspend, resume, abort, abort_safely, depress_abort: accepted */
	case 3613: case 3617: case 3604: case 3611: case 3600:		/* thread_set_exception_ports, policy_set, set_state, set_special_port, terminate: accepted, nothing happens */
		if (dest->kind != KP_THREAD) return (false);
		mig_reply_data(m, id, 0, NULL, 0);
		return (true);
	case 4816:					/* mach_vm_region(task, *address, *size, flavor, *info, *infoCnt, *object_name): VM_REGION_BASIC_INFO_64 (9) */
		{
			uint64_t addr, rstart = 0, rsize = 0;
			uint32_t flavor, buf9[9];
			vm_map_t map;
			vm_map_entry_t ent;
			bool found = false;
			struct kmsg *r;
			uint8_t *rd;

			if (m->size < 48 || m->reply.port == NULL || m->reply.port->dead) { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			addr = rd64(d, 32); flavor = rd32(d, 40);
			if (flavor != 9 || p->p_vmspace == NULL) { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			memset(buf9, 0, sizeof(buf9));
			map = &p->p_vmspace->vm_map;
			vm_map_lock_read(map);
			VM_MAP_ENTRY_FOREACH(ent, map) {
				if (ent->end <= addr || (ent->eflags & MAP_ENTRY_IS_SUB_MAP) != 0)
					continue;
				rstart = ent->start; rsize = ent->end - ent->start;
				buf9[0] = ent->protection & 7; buf9[1] = ent->max_protection & 7; buf9[2] = ent->inheritance == VM_INHERIT_SHARE ? 0 : 1;
				buf9[3] = (ent->eflags & MAP_ENTRY_COW) == 0 ? 1 : 0;			/* shared */
				memcpy(&buf9[5], &ent->offset, 8);
				found = true;
				break;
			}
			vm_map_unlock_read(map);
			if (!found) { mig_reply_data(m, id, 1 /* KERN_INVALID_ADDRESS */, NULL, 0); return (true); }
			r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->data = rd = malloc(104, M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->size = 104;
			wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
			wr32(rd, 4, 104); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
			rd[28 + 10] = MM_MOVE_SEND; rd[28 + 11] = 0;			/* object_name: the null port */
			memcpy(rd + 48, &rstart, 8); memcpy(rd + 56, &rsize, 8);
			wr32(rd, 64, 9);
			memcpy(rd + 68, buf9, sizeof(buf9));
			TAILQ_INSERT_TAIL(&m->reply.port->q, r, link);
			m->reply.port->qn++; kq_post(m->reply.port);
			wakeup(m->reply.port);
			return (true);
		}
	case 4808:					/* mach_vm_read_overwrite(task, address, size, data, *outsize): into the memory of the caller */
	case 4804:					/* mach_vm_read(task, address, size, *data[], *dataCnt): out of line memory in the reply */
		{
			uint64_t addr, size, dst = 0;
			uint8_t *kb;
			int error;

			if (m->size < 48) { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			addr = rd64(d, 32); size = rd64(d, 40);
			if (id == 4808) { if (m->size < 56) { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); } dst = rd64(d, 48); }
			if (size == 0 || size > (1 << 20)) { mig_reply_data(m, id, size == 0 ? 0 : KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			kb = malloc(size, M_DARWIN_IPC, M_WAITOK);
			sx_xunlock(&km_sx);					/* the memory of a process is read with the lock dropped (it can fault and sleep) */
			error = p == td->td_proc ? copyin((void *)(uintptr_t)addr, kb, size) : proc_readmem(td, p, (vm_offset_t)addr, kb, size) == (ssize_t)size ? 0 : EFAULT;
			if (error == 0 && id == 4808)
				error = copyout(kb, (void *)(uintptr_t)dst, size);
			sx_xlock(&km_sx);
			if (error != 0) { free(kb, M_DARWIN_IPC); mig_reply_data(m, id, 1 /* KERN_INVALID_ADDRESS */, NULL, 0); return (true); }
			if (id == 4808) {
				uint64_t os = size;

				free(kb, M_DARWIN_IPC);
				mig_reply_data(m, id, 0, &os, 8);
			} else if (m->reply.port != NULL && !m->reply.port->dead) {
				struct kmsg *r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
				uint8_t *rd = r->data = malloc(56, M_DARWIN_IPC, M_WAITOK | M_ZERO);

				r->size = 56;
				wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
				wr32(rd, 4, 56); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
				r->ool[0].type = 1; r->ool[0].off = 28; r->ool[0].size = (uint32_t)size; r->ool[0].data = kb;
				r->nool = 1;
				wr32(rd, 28 + 12, (uint32_t)size); rd[28 + 11] = 1;
				wr32(rd, 52, (uint32_t)size);
				TAILQ_INSERT_TAIL(&m->reply.port->q, r, link);
				m->reply.port->qn++; kq_post(m->reply.port);
				wakeup(m->reply.port);
			} else
				free(kb, M_DARWIN_IPC);
			return (true);
		}
	case 4806:					/* mach_vm_write(task, address, data (out of line), dataCnt) */
		{
			uint64_t addr;
			int error;

			if (m->size < 64 || m->nool != 1 || m->ool[0].type != 1) { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			addr = rd64(d, 52);			/* hdr 24, ndesc 4, descriptor 16, NDR 8, then the address */
			sx_xunlock(&km_sx);
			error = p == td->td_proc ? copyout(m->ool[0].data, (void *)(uintptr_t)addr, m->ool[0].size) : proc_writemem(td, p, (vm_offset_t)addr, m->ool[0].data, m->ool[0].size) == (ssize_t)m->ool[0].size ? 0 : EFAULT;
			sx_xlock(&km_sx);
			mig_reply_data(m, id, error == 0 ? 0 : 1, NULL, 0);
			return (true);
		}
	case 1000:					/* clock_get_time(clock, *cur_time): a mach_timespec_t (two 32 bit numbers); the clock port has the calendar clock (kpid 1) or the system clock (0) */
		{
			struct timespec ts;
			uint32_t tv[2];

			if (dest->kind != KP_CLOCK) return (false);
			if (dest->kpid == 1) nanotime(&ts); else nanouptime(&ts);
			tv[0] = (uint32_t)ts.tv_sec; tv[1] = (uint32_t)ts.tv_nsec;
			mig_reply_data(m, id, 0, tv, 8);
			return (true);
		}
	case 1001:					/* clock_get_attributes(clock, flavor, *attr, *count): CLOCK_GET_TIME_RES (1): 1 ns (a count of 1) */
		{
			uint32_t buf[2] = { 1, 1 };

			if (dest->kind != KP_CLOCK) return (false);
			mig_reply_data(m, id, 0, buf, sizeof(buf));
			return (true);
		}
	case 3204:					/* mach_port_allocate(task, right, *name) */
		{
			uint32_t right = m->size >= 36 ? rd32(d, 32) : 99;
			struct kport *np;
			struct kent *ne;

			if (right != MACH_PORT_RIGHT_RECEIVE && right != MACH_PORT_RIGHT_PORT_SET) { mig_reply_data(m, id, KERN_INVALID_VALUE, NULL, 0); return (true); }
			np = kp_new(right == MACH_PORT_RIGHT_PORT_SET ? KP_SET : KP_PORT);
			ne = ent_add(t, np, MM_MOVE_RECEIVE);
			np->refs--;
			if (ne == NULL) { kp_unref(np); mig_reply_data(m, id, KERN_NO_SPACE, NULL, 0); return (true); }
			a = ne->name;
			mig_reply_data(m, id, 0, &a, 4);
			return (true);
		}
	case 3240:					/* mach_port_is_connection_for_service(task, connection_port, service_port, *filter_policy_id): libxpc asks it for every connection that comes to a service; here every connection is for the service, with no filter */
		{
			uint64_t fid = 0;

			mig_reply_data(m, id, 0, &fid, 8);
			return (true);
		}
	case 3205: case 3232:				/* mach_port_destroy(task, name), mach_port_destruct(task, name, srdelta, guard): every right of the name goes */
	case 3206:					/* mach_port_deallocate(task, name): one send or send-once right, or the dead name */
	case 3208:					/* mach_port_mod_refs(task, name, right, delta) */
	case 3207:					/* mach_port_get_refs(task, name, right, *refs) */
		{
			struct kent *e = m->size >= 36 ? ent_by_name(t, rd32(d, 32)) : NULL;
			int right = m->size >= 40 ? (int)rd32(d, 36) : 0, delta = m->size >= 44 ? (int)rd32(d, 40) : 0, kr = KERN_SUCCESS;

			if (e == NULL) { mig_reply_data(m, id, KERN_INVALID_NAME, NULL, 0); return (true); }
			if (id == 3205 || id == 3232) {
				if (e->recv) { e->recv = false; port_die(e->port); }
				e->send = 0; e->sonce = 0;
			} else if (id == 3206) {
				if (e->send > 0) e->send--;
				else if (e->sonce > 0) e->sonce--;
				else if (e->recv) kr = KERN_INVALID_RIGHT;
			} else if (id == 3208) {
				kr = KERN_INVALID_RIGHT;
				if (right == MACH_PORT_RIGHT_RECEIVE && e->recv && delta < 0) { e->recv = false; port_die(e->port); kr = KERN_SUCCESS; }
				else if (right == MACH_PORT_RIGHT_SEND && e->send > 0 && e->send + delta >= 0) { e->send += delta; kr = KERN_SUCCESS; }
				else if (right == MACH_PORT_RIGHT_SEND_ONCE && e->sonce > 0 && e->sonce + delta >= 0) { e->sonce += delta; kr = KERN_SUCCESS; }
			} else {
				a = right == MACH_PORT_RIGHT_SEND ? e->send : right == MACH_PORT_RIGHT_RECEIVE ? (e->recv ? 1 : 0) : right == MACH_PORT_RIGHT_SEND_ONCE ? e->sonce : 0;
				if (right == 4) a = (e->port->dead && !e->recv) ? 1 : 0;	/* MACH_PORT_RIGHT_DEAD_NAME */
				if (darwin_debug >= 1) {		/* a program that asks for the rights of its ports in a loop: where from (the frames of the user stack, once in 200000 calls) */
					static u_long calls;

					if ((++calls % 200000) == 1) {
						uint64_t fp = td->td_frame->tf_rbp, ret[6];
						int q;

						for (q = 0; q < 6; q++) {
							ret[q] = 0;
							if (fp == 0 || copyin((void *)(fp + 8), &ret[q], 8) != 0 || copyin((void *)fp, &fp, 8) != 0)
								break;
						}
						printf("darwinator: %s[%d]: mach_port_get_refs loop (call %lu), frames %#lx %#lx %#lx %#lx %#lx %#lx\n", td->td_proc->p_comm, td->td_proc->p_pid, calls, (u_long)ret[0], (u_long)ret[1], (u_long)ret[2], (u_long)ret[3], (u_long)ret[4], (u_long)ret[5]);
					}
				}
				mig_reply_data(m, id, 0, &a, 4);
				return (true);
			}
			ent_drop_if_empty(e);
			ns_check();
			mig_reply_data(m, id, kr, NULL, 0);
			return (true);
		}
	case 3222:					/* mach_port_get_srights(task, name, *srights) */
		{
			struct kent *e = m->size >= 36 ? ent_by_name(t, rd32(d, 32)) : NULL;

			if (e == NULL || !e->recv) { mig_reply_data(m, id, e == NULL ? KERN_INVALID_NAME : KERN_INVALID_RIGHT, NULL, 0); return (true); }
			a = (uint32_t)ns_sends(e->port);
			mig_reply_data(m, id, 0, &a, 4);
			return (true);
		}
	case 3210: case 3216:				/* mach_port_set_mscount, mach_port_set_seqno: accepted */
	case 3218:					/* mach_port_set_attributes: accepted */
	case 8000: case 8001:				/* task_restartable_ranges_register (the ranges are not used), task_restartable_ranges_synchronize: libobjc needs them to answer 0 */
	case 3407: case 3408:				/* task_suspend, task_resume */
	case 3406:					/* task_set_info */
	case 3413:					/* task_set_exception_ports: the handlers are not called here */
	case 3420: case 3421: case 3423:		/* task_policy_set/get/task_policy */
	case 3444: case 3445:				/* task_register_dyld_image_infos / unregister */
	case 3447: case 3448:				/* dyld shared cache image info / state */
		mig_reply_data(m, id, 0, NULL, 0);
		return (true);
	case 3229:					/* mach_port_set_context(task, name, context u64): the context of a receive right */
		{
			struct kent *e = m->size >= 44 ? ent_by_name(t, rd32(d, 32)) : NULL;

			if (e == NULL || !e->recv) { mig_reply_data(m, id, e == NULL ? KERN_INVALID_NAME : KERN_INVALID_RIGHT, NULL, 0); return (true); }
			e->port->context = (uint64_t)rd32(d, 36) | ((uint64_t)rd32(d, 40) << 32);
			if (darwin_debug >= 1)
				printf("darwinator: %s[%d]: mach_port_set_context(name %#x, %#lx)\n", td->td_proc->p_comm, td->td_proc->p_pid, rd32(d, 32), (u_long)e->port->context);
			mig_reply_data(m, id, 0, NULL, 0);
			return (true);
		}
	case 3228:					/* mach_port_get_context(task, name, *context: 8 bytes) */
		{
			struct kent *e = m->size >= 36 ? ent_by_name(t, rd32(d, 32)) : NULL;
			uint64_t cv = e != NULL && e->recv ? e->port->context : 0;

			if (e == NULL || !e->recv) { mig_reply_data(m, id, e == NULL ? KERN_INVALID_NAME : KERN_INVALID_RIGHT, NULL, 0); return (true); }
			mig_reply_data(m, id, 0, &cv, 8);
			return (true);
		}
	case 3405:					/* task_info(task, flavor, *info, *count) */
		a = m->size >= 40 ? rd32(d, 32) : 0;
		if (darwin_debug >= 2)
			printf("darwinator: task_info flavor %u count %u\n", a, m->size >= 44 ? rd32(d, 36) : 0);
		{
			uint64_t vsz = 0, rss = 0;

			if (p->p_vmspace != NULL) { vsz = p->p_vmspace->vm_map.size; rss = (uint64_t)pmap_resident_count(vmspace_pmap(p->p_vmspace)) * PAGE_SIZE; }
			if (a == 20) {						/* MACH_TASK_BASIC_INFO: virtual, resident, resident max (u64 each), user and system time (2 ints each), policy, suspend count */
				uint64_t q[3] = { vsz, rss, rss };

				memcpy(out, q, 24); out[6] = out[7] = out[8] = out[9] = 0; out[10] = 1; out[11] = 0; n = 12;
			} else if (a == 5) {					/* TASK_BASIC_INFO_64: the same without the maximum */
				uint64_t q[2] = { vsz, rss };

				memcpy(out, q, 16); out[4] = out[5] = out[6] = out[7] = 0; out[8] = 1; out[9] = 0; n = 10;
			} else if (a == 15) {					/* TASK_AUDIT_TOKEN: audit_token_t, eight 32 bit words: auid euid egid ruid rgid pid asid pidversion */
				out[0] = 0xffffffffu; out[1] = td->td_ucred->cr_uid; out[2] = td->td_ucred->cr_gid; out[3] = td->td_ucred->cr_ruid; out[4] = td->td_ucred->cr_rgid;
				out[5] = (uint32_t)p->p_pid; out[6] = (uint32_t)p->p_pid; out[7] = 1; n = 8;
			} else if (a == 17) {					/* TASK_DYLD_INFO: all_image_info_addr (u64), size (u64), format (int): nothing registered */
				n = 5;
			} else { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
		}
		{
			uint32_t buf[1 + 16];

			buf[0] = (uint32_t)n;
			memcpy(buf + 1, out, n * 4);
			mig_reply_data(m, id, 0, buf, (n + 1) * 4);
		}
		return (true);
	case 200:					/* host_info(host, flavor, *info, *count): HOST_BASIC_INFO (1), HOST_SCHED_INFO (3), HOST_PRIORITY_INFO (5): the values of a Mac (tools/macprobe) */
		a = m->size >= 40 ? rd32(d, 32) : 0;
		if (a == 3 || a == 5 || a == 7 || a == 8) {
			uint32_t buf[1 + 8];

			memset(buf, 0, sizeof(buf));
			if (a == 3) { buf[0] = 2; buf[1] = 10; buf[2] = 10; }					/* min_timeout, min_quantum */
			else if (a == 5) {
				static const uint32_t pri[8] = { 80, 80, 64, 31, 0, 0, 0, 79 };	/* kernel, system, server, user, depress, idle, minimum, maximum */

				buf[0] = 8; memcpy(&buf[1], pri, sizeof(pri));
			}							/* flavors 7 and 8: success, nothing to report (count 0) */
			mig_reply_data(m, id, 0, buf, (buf[0] + 1) * 4);
			return (true);
		}
		if (a != 1) { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
		{
			uint32_t buf[1 + 12];
			uint64_t mem = (uint64_t)ptoa((uint64_t)physmem);

			memset(buf, 0, sizeof(buf));
			buf[0] = 12; buf[1] = (uint32_t)mp_ncpus; buf[2] = (uint32_t)mp_ncpus; buf[3] = (uint32_t)MIN(mem, (uint64_t)INT32_MAX + 1);
#if defined(__amd64__)
			buf[4] = 7;			/* CPU_TYPE_X86: what host_info says on an Intel Mac (not the 64 bit type) */
			buf[5] = 8;			/* CPU_SUBTYPE_X86_64_H (Haswell): dyld chooses the x86_64h cache when this is 8 */
#else
			buf[4] = 0x0100000c;		/* CPU_TYPE_ARM64 */
#endif
			buf[6] = 1;			/* cpu_threadtype */
			buf[7] = (uint32_t)mp_ncpus; buf[8] = (uint32_t)mp_ncpus; buf[9] = (uint32_t)mp_ncpus; buf[10] = (uint32_t)mp_ncpus;
			memcpy(&buf[11], &mem, 8);
			mig_reply_data(m, id, 0, buf, sizeof(buf));
		}
		return (true);
	case 216: case 219:				/* host_statistics / host_statistics64(host, flavor, *info, *count) */
		{
			uint32_t buf[1 + 40];
			uint64_t q[16];
			int cnt = 0, i;

			a = m->size >= 40 ? rd32(d, 32) : 0;
			memset(buf, 0, sizeof(buf));
			if (a == 4 && id == 219) {			/* HOST_VM_INFO64: vm_statistics64, 38 ints */
				uint32_t *v = buf + 1;
				uint64_t w[9] = { VM_CNT_FETCH(v_zfod), 0, VM_CNT_FETCH(v_vnodepgsin), VM_CNT_FETCH(v_vnodepgsout), VM_CNT_FETCH(v_vm_faults), VM_CNT_FETCH(v_cow_faults), 0, 0, 0 };

				v[0] = vm_free_count(); v[1] = (uint32_t)physmem - vm_free_count() - vm_wire_count(); v[2] = 0; v[3] = vm_wire_count();
				memcpy(v + 4, w, sizeof(w));			/* zero_fill, reactivations, pageins, pageouts, faults, cow_faults, lookups, hits, purges */
				cnt = 38;
			} else if (a == 2 && id == 216) {			/* HOST_VM_INFO: vm_statistics, 15 ints */
				uint32_t *v = buf + 1;

				v[0] = vm_free_count(); v[1] = (uint32_t)physmem - vm_free_count() - vm_wire_count(); v[2] = 0; v[3] = vm_wire_count();
				v[4] = (uint32_t)VM_CNT_FETCH(v_zfod); v[6] = (uint32_t)VM_CNT_FETCH(v_vnodepgsin); v[7] = (uint32_t)VM_CNT_FETCH(v_vnodepgsout);
				v[8] = (uint32_t)VM_CNT_FETCH(v_vm_faults); v[9] = (uint32_t)VM_CNT_FETCH(v_cow_faults);
				cnt = 15;
			} else if (a == 3) {					/* HOST_CPU_LOAD_INFO: ticks user, system, idle, nice */
				memset(q, 0, sizeof(q));
				CPU_FOREACH(i) {
					struct pcpu *pc = pcpu_find(i);

					if (pc == NULL)
						continue;
					q[0] += pc->pc_cp_time[CP_USER] + 0; q[1] += pc->pc_cp_time[CP_SYS] + pc->pc_cp_time[CP_INTR];
					q[2] += pc->pc_cp_time[CP_IDLE]; q[3] += pc->pc_cp_time[CP_NICE];
				}
				for (i = 0; i < 4; i++)
					buf[1 + i] = (uint32_t)q[i];
				cnt = 4;
			} else { mig_reply_data(m, id, KERN_INVALID_ARGUMENT, NULL, 0); return (true); }
			buf[0] = (uint32_t)cnt;
			mig_reply_data(m, id, 0, buf, (cnt + 1) * 4);
		}
		return (true);
	case 202:					/* _host_page_size(host, *page_size: 8 bytes) */
		{
			uint64_t ps = PAGE_SIZE;

			mig_reply_data(m, id, 0, &ps, 8);
		}
		return (true);
	default:
		(void)b;
		return (false);
	}
}

/*
 * The bootstrap port: libxpc and the libraries send their requests to launchd here as XPC messages ("@XPC", version 5, then a dictionary: type 0xf000, size, count, and for each
 * entry the key (a C string padded to 4) and the value (a type word and the data)).  The request of a look-up has the keys handle, instance, flags, name, type, targetpid and
 * domain-port, and the message id 0xcf.  The kernel answers as a launchd without any service: an empty dictionary, which libxpc reads as "unknown service".
 */
static void
bootstrap_server(struct thread *td, struct ktask *t, struct kport *dest, struct kmsg *m)
{
	uint32_t i, n = MIN(m->size, 256);
	struct kmsg *r;
	struct kport *rp = m->reply.port;
	uint8_t *rd;

	(void)t; (void)dest;
	if (darwin_debug >= 1) {
		printf("darwinator: %s[%d]: message to the bootstrap port: id %#x size %u bits %#x:", td->td_proc->p_comm, td->td_proc->p_pid, rd32(m->data, 20), m->size, rd32(m->data, 0));
		for (i = 0; i < n; i += 4)
			printf(" %08x", rd32(m->data, i));
		printf("\n");
	}
	if (rp == NULL || rp->dead)
		return;
	/* an empty dictionary for the look-up (0xcf); the routines 0x320-0x325 (check-in and registration) get {"error": ESRCH}, which libxpc reports to the caller instead of aborting */
	r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->data = rd = malloc(64, M_DARWIN_IPC, M_WAITOK | M_ZERO);
	wr32(rd, 0, MM_MOVE_SEND_ONCE);
	wr32(rd, 20, 0x20000000);				/* the id of an XPC reply */
	wr32(rd, 24, 0x40585043);				/* "CPX@" */
	wr32(rd, 28, 5);					/* version */
	wr32(rd, 32, 0xf000);					/* a dictionary ... */
	if ((rd32(m->data, 20) & 0xfff) >= 0x320) {
		r->size = 64;
		wr32(rd, 36, 24);				/* ... of 24 bytes ... */
		wr32(rd, 40, 1);				/* ... with one entry: */
		memcpy(rd + 44, "error", 6);			/* the key, padded to 8 */
		wr32(rd, 52, 0x3000);				/* int64 */
		wr32(rd, 56, 3);				/* ESRCH */
	} else {
		r->size = 44;
		wr32(rd, 36, 4);
		wr32(rd, 40, 0);
	}
	wr32(rd, 4, r->size);
	TAILQ_INSERT_TAIL(&rp->q, r, link);
	rp->qn++; kq_post(rp);
	wakeup(rp);
}


/*
 * The IOKit registry of this system: a table of nodes (the platform, the devices that programs ask for), the objects (an entry or an iterator) are ports of kind KP_IOOBJ.  The routines are the ones of
 * the kernel subsystem "iokit" (device.defs, 2800..2890, found with migcom): libIOKit of the Mac sends them to the port of host_get_io_main.  Enough for what the window server and the Dock look up;
 * a request that is not understood fails (and is logged at kern.darwinator.debug >= 1).
 */
struct ionode { const char *name, *cls; int parent; };
static const struct ionode io_nodes[] = {
	{ "Root", "IORegistryEntry", -1 },
	{ "VirtualMac", "IOPlatformExpertDevice", 0 },
	{ "IOHIDSystem", "IOHIDSystem", 1 },
	{ "IOResources", "IOResources", 0 },
};
#define	IO_NNODES	((int)nitems(io_nodes))
#define	IO_ITER_BASE	0x1000
#define	IO_ITER_N	128
#define	IO_CONN_BASE	0x2000			/* a connection (a user client made by io_service_open_extended): the node is kpid - IO_CONN_BASE */
static struct { int list[16]; int n, pos; } io_iters[IO_ITER_N];
static int io_iter_next_slot;

static bool
io_node_matches(int i, const char *cls)
{
	return (strcmp(io_nodes[i].cls, cls) == 0 || strcmp(io_nodes[i].name, cls) == 0);
}

/* the text of the value of a key in a matching dictionary in XML: <key>K</key><string>V</string> */
static bool
io_xml_value(const char *xml, const char *key, char *val, size_t vsz)
{
	char pat[96];
	const char *p, *q;

	snprintf(pat, sizeof(pat), "<key>%s</key><string>", key);
	if ((p = strstr(xml, pat)) == NULL)
		return (false);
	p += strlen(pat);
	if ((q = strstr(p, "</string>")) == NULL || (size_t)(q - p) >= vsz)
		return (false);
	memcpy(val, p, q - p); val[q - p] = 0;
	return (true);
}

/* a matching dictionary in the binary form of OSSerialize (the signature 0xd3, then words: the type in 0x7f000000, the length in the low bits, the end in 0x80000000; the strings are padded to 4): the value that follows the key */
static bool
io_bin_value(const uint8_t *b, uint32_t len, const char *key, char *val, size_t vsz)
{
	uint32_t pos = 4, w, l;
	bool take = false;

	if (len < 8 || rd32(b, 0) != 0xd3)
		return (false);
	while (pos + 4 <= len) {
		uint32_t ty;

		w = rd32(b, pos); pos += 4;
		l = w & 0xffffff;
		ty = (w >> 24) & 0x7f;
		if (ty == 0x08 || ty == 0x09) {			/* a symbol or a string (the length includes the NUL) */
			if (pos + l > len) return (false);
			if (take) { if (l >= vsz) return (false); memcpy(val, b + pos, l); val[l] = 0; return (true); }
			if (l == strlen(key) + 1 && memcmp(b + pos, key, l) == 0) take = true;
			pos += (l + 3) & ~3u;
		} else if (ty == 0x0a) {			/* data */
			pos += (l + 3) & ~3u;
		} else if (ty == 0x04) {			/* a number: 8 bytes of data */
			pos += 8;
		}						/* a dictionary (1), array (2), set (3): the count, the members follow; a boolean (0x0b) or an object (0x0c): nothing */
		if (ty != 0x08 && ty != 0x09) take = false;
	}
	return (false);
}

static struct kport *
io_new_iter(int *list, int n)
{
	struct kport *ip = kp_new(KP_IOOBJ);
	int slot = io_iter_next_slot++ % IO_ITER_N, i;

	io_iters[slot].n = n > 16 ? 16 : n; io_iters[slot].pos = 0;
	for (i = 0; i < io_iters[slot].n; i++) io_iters[slot].list[i] = list[i];
	ip->kpid = IO_ITER_BASE + slot;
	return (ip);
}

/* a reply with the one out parameter of the type io_name_t / io_string_t: the offset (0), the count and the bytes */
static void
io_reply_string(struct kmsg *m, int id, const char *str)
{
	uint8_t buf[8 + 520];
	uint32_t len = (uint32_t)strlen(str) + 1;

	memset(buf, 0, sizeof(buf));
	wr32(buf, 0, 0); wr32(buf, 4, len);
	memcpy(buf + 8, str, len);
	mig_reply_data(m, id, 0, buf, 8 + len);
}

static void
io_reply_null_port(struct kmsg *m, int id)
{
	struct kport *rp = m->reply.port;
	struct kmsg *r;
	uint8_t *rd;

	if (rp == NULL || rp->dead)
		return;
	r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->data = rd = malloc(40, M_DARWIN_IPC, M_WAITOK | M_ZERO);
	r->size = 40;
	wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
	wr32(rd, 4, 40); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
	rd[28 + 10] = MM_MOVE_SEND_ONCE; rd[28 + 11] = 0;
	TAILQ_INSERT_TAIL(&rp->q, r, link);
	rp->qn++; kq_post(rp);
	wakeup(rp);
}

/* the request has the form NDR(8) + offset(4) + count(4) + the string at 40: the string of an inband parameter */
static const char *
io_req_string(struct kmsg *m, uint32_t at)
{
	static char tmp[512];
	uint32_t cnt;

	if (m->size < at + 8)
		return ("");
	cnt = rd32(m->data, at + 4);
	if (cnt > 511 || at + 8 + cnt > m->size)
		return ("");
	memcpy(tmp, m->data + at + 8, cnt); tmp[cnt] = 0;
	return (tmp);
}

static void
io_serve(struct ktask *t, struct kport *dest, struct kmsg *m, int id)
{
	int node = dest->kind == KP_IOOBJ && dest->kpid < IO_ITER_BASE ? (int)dest->kpid : dest->kind == KP_IOOBJ && dest->kpid >= IO_CONN_BASE ? (int)(dest->kpid - IO_CONN_BASE) : -1;
	int slot = dest->kind == KP_IOOBJ && dest->kpid >= IO_ITER_BASE && dest->kpid < IO_CONN_BASE ? (int)(dest->kpid - IO_ITER_BASE) : -1;
	bool conn = dest->kind == KP_IOOBJ && dest->kpid >= IO_CONN_BASE;
	struct kport *np;
	int list[16], n, i;

	switch (id) {
	case 2877: {					/* io_server_version(master, *version): the version of the IOKit server (the one of macOS 12: 20190423) */
		uint64_t v = 20190423;

		mig_reply_data(m, id, 0, &v, 8);
		return;
	}
	case 2827:					/* io_registry_get_root_entry(master, *root) */
		np = kp_new(KP_IOOBJ); np->kpid = 0;
		mig_reply(t, m, id, 0, np, NULL, 0, NULL, 0);
		kp_unref(np);
		return;
	case 2809: {					/* io_registry_entry_from_path(master, path, *entry): "IOService:/" and the names of the nodes below it */
		const char *path = io_req_string(m, 32);
		const char *nm;

		nm = strchr(path, ':') != NULL ? strchr(path, ':') + 1 : path;
		n = -1;
		if (strcmp(nm, "/") == 0 || *nm == 0) n = 0;
		else for (i = 0; i < IO_NNODES; i++) {
			size_t l = strlen(io_nodes[i].name);

			if (strncmp(nm, "/", 1) == 0 && strlen(nm) >= l && strcmp(nm + strlen(nm) - l, io_nodes[i].name) == 0) { n = i; break; }
		}
		if (n < 0) { mig_reply(t, m, id, (int)0xe00002c0 /* kIOReturnNoDevice */, NULL, NULL, 0, NULL, 0); return; }
		np = kp_new(KP_IOOBJ); np->kpid = n;
		mig_reply(t, m, id, 0, np, NULL, 0, NULL, 0);
		kp_unref(np);
		return;
	}
	case 2832: case 2890:				/* io_service_wait_quiet(service, wait_time): nothing is busy */
		mig_reply_data(m, id, 0, NULL, 0);
		return;
	case 2804: case 2873: case 2880: case 2881: {	/* io_service_get_matching_services / _service (XML) and the _bin forms (binary): the nodes of the class that the dictionary names */
		char cls[96];
		const char *xml = io_req_string(m, 32);
		bool found;

		n = 0;
		if (id == 2880 || id == 2881)
			found = m->size >= 36 && io_bin_value(m->data + 36, MIN(rd32(m->data, 32), m->size - 36), "IOProviderClass", cls, sizeof(cls));
		else
			found = io_xml_value(xml, "IOProviderClass", cls, sizeof(cls)) || io_xml_value(xml, "IOClass", cls, sizeof(cls)) || io_xml_value(xml, "IONameMatch", cls, sizeof(cls));
		if (found)
			for (i = 0; i < IO_NNODES && n < 16; i++)
				if (io_node_matches(i, cls)) list[n++] = i;
		if (darwin_debug >= 1)
			printf("darwinator: IOKit: matching %s: %d node(s)\n", found ? cls : "(no class)", n);
		if (darwin_debug >= 1 && !found) {
			uint32_t w;

			for (w = 24; w + 4 <= m->size && w < 80; w += 4)
				printf("darwinator:   [%u] %08x\n", w, rd32(m->data, w));
		}
		if (id == 2873 || id == 2880) {			/* a single service: the first, or none */
			if (n == 0) { io_reply_null_port(m, id); return; }
			np = kp_new(KP_IOOBJ); np->kpid = list[0];
		} else
			np = io_new_iter(list, n);
		mig_reply(t, m, id, 0, np, NULL, 0, NULL, 0);
		kp_unref(np);
		return;
	}
	case 2802:					/* io_iterator_next(iterator, *object): the next entry or the null port at the end */
		if (slot < 0) break;
		if (io_iters[slot].pos >= io_iters[slot].n) { io_reply_null_port(m, id); return; }
		np = kp_new(KP_IOOBJ); np->kpid = io_iters[slot].list[io_iters[slot].pos++];
		mig_reply(t, m, id, 0, np, NULL, 0, NULL, 0);
		kp_unref(np);
		return;
	case 2803:					/* io_iterator_reset */
		if (slot >= 0) io_iters[slot].pos = 0;
		mig_reply_data(m, id, 0, NULL, 0);
		return;
	case 2834: {					/* io_iterator_is_valid(iterator, *is_valid) */
		uint32_t one = 1;

		mig_reply_data(m, id, 0, &one, 4);
		return;
	}
	case 2810: case 2800:				/* io_registry_entry_get_name(entry, *name) / io_object_get_class(object, *class) */
		if (node < 0) break;
		io_reply_string(m, id, id == 2810 ? io_nodes[node].name : io_nodes[node].cls);
		return;
	case 2826: {					/* io_registry_entry_get_path(entry, plane, *path) */
		char path[256] = "";
		int chain[8], c = 0, k;

		if (node < 0) break;
		for (k = node; k > 0 && c < 8; k = io_nodes[k].parent) chain[c++] = k;
		snprintf(path, sizeof(path), "IOService:/");
		for (k = c - 1; k >= 0; k--) { strlcat(path, io_nodes[chain[k]].name, sizeof(path)); if (k > 0) strlcat(path, "/", sizeof(path)); }
		io_reply_string(m, id, path);
		return;
	}
	case 2813: case 2814:				/* io_registry_entry_get_child_iterator / _parent_iterator(entry, plane, *iterator) */
		if (node < 0) break;
		n = 0;
		if (id == 2813) { for (i = 0; i < IO_NNODES && n < 16; i++) if (io_nodes[i].parent == node) list[n++] = i; }
		else if (io_nodes[node].parent >= 0) list[n++] = io_nodes[node].parent;
		np = io_new_iter(list, n);
		mig_reply(t, m, id, 0, np, NULL, 0, NULL, 0);
		kp_unref(np);
		return;
	case 2801: {					/* io_object_conforms_to(object, class, *conforms) */
		uint32_t yes = 0;

		if (node >= 0) { const char *cn = io_req_string(m, 32); yes = io_node_matches(node, cn) ? 1 : 0; }
		mig_reply_data(m, id, 0, &yes, 4);
		return;
	}
	case 2817:					/* io_connect_get_service(connection, *service): the node that the connection is to */
		if (!conn) break;
		np = kp_new(KP_IOOBJ); np->kpid = node;
		mig_reply(t, m, id, 0, np, NULL, 0, NULL, 0);
		kp_unref(np);
		return;
	case 2828: case 2821:				/* io_registry_entry_set_properties / io_connect_set_properties(entry, properties, *result): taken, nothing is kept */
	{
		uint32_t zero = 0;

		mig_reply_data(m, id, 0, &zero, 4);
		return;
	}
	case 2856: {					/* io_service_get_state(service, *state, *busy_state, *accumulated_busy_time): registered, matched, active; not busy */
		uint8_t st[20];

		memset(st, 0, sizeof(st));
		st[0] = 7;
		mig_reply_data(m, id, 0, st, sizeof(st));
		return;
	}
	case 2862: {					/* io_service_open_extended(service, task, type, ndr, properties, *connection, *result): a connection port, kept as an object of the node */
		struct kport *cp = kp_new(KP_IOOBJ);
		struct kmsg *r;
		uint8_t *rd;

		if (node < 0 || m->reply.port == NULL || m->reply.port->dead) { kp_unref(cp); break; }
		cp->kpid = IO_CONN_BASE + node;
		r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
		r->data = rd = malloc(52, M_DARWIN_IPC, M_WAITOK | M_ZERO);
		r->size = 52;
		wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
		wr32(rd, 4, 52); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
		r->carry[0].port = cp; cp->refs++; r->carry[0].disp = MM_MOVE_SEND; r->carry[0].nameoff = 28;
		r->ncarry = 1;
		rd[28 + 10] = MM_MOVE_SEND; rd[28 + 11] = 0;
		TAILQ_INSERT_TAIL(&m->reply.port->q, r, link);
		m->reply.port->qn++; kq_post(m->reply.port);
		wakeup(m->reply.port);
		kp_unref(cp);
		if (darwin_debug >= 1)
			printf("darwinator: IOKit: a connection to %s\n", io_nodes[node].name);
		return;
	}
	case 2865: {					/* io_connect_method(connection, selector, scalars, inband, ool, ...): every method of a user client works and gives nothing back */
		uint8_t z[16];

		if (!conn) break;
		memset(z, 0, sizeof(z));
		if (darwin_debug >= 1)
			printf("darwinator: IOKit: method %u of the connection to %s\n", m->size >= 36 ? rd32(m->data, 32) : 0, io_nodes[node].name);
		mig_reply_data(m, id, 0, z, sizeof(z));
		return;
	}
	case 2867: case 2816:				/* io_connect_set_notification_port / io_service_close */
		mig_reply_data(m, id, 0, NULL, 0);
		return;
	case 2830: {					/* io_object_get_retain_count */
		uint32_t one = 1;

		mig_reply_data(m, id, 0, &one, 4);
		return;
	}
	default:
		break;
	}
	if (darwin_debug >= 1)
		printf("darwinator: IOKit request %d is not served (it fails)\n", id);
	mig_reply(t, m, id, (int)0xe00002c7 /* kIOReturnUnsupported */, NULL, NULL, 0, NULL, 0);
}

static void
mig_server(struct thread *td, struct ktask *t, struct kport *dest, struct kmsg *m)
{
	uint8_t *d = m->data;
	int id = (int)rd32(d, 20);
	uint32_t which;

	if (mig_info(td, t, dest, m, id))
		return;
	if (dest->kind == KP_HOST && id == 205) {			/* host_get_io_main(host, *io_main): the port of IOKit (the programs that look for a display, a HID device or a GPU ask for it first) */
		struct kport *ip = kp_new(KP_IOKIT);

		mig_reply(t, m, id, 0, ip, NULL, 0, NULL, 0);
		kp_unref(ip);
		return;
	}
	if (dest->kind == KP_IOKIT || dest->kind == KP_IOOBJ) {			/* IOKit's routines (the subsystem 2800): the registry of io_serve */
		if (darwin_debug >= 1) {
			char txt[161];
			uint32_t k, n = 0;

			/* the printable text of the request (the matching dictionary, the path or the class name travel as text) */
			for (k = 24; k < m->size && n < 160; k++)
				txt[n++] = (d[k] >= 32 && d[k] < 127) ? (char)d[k] : '.';
			for (k = 0; k < (uint32_t)m->nool && n < 160; k++)
				if (m->ool[k].type == 1 && m->ool[k].data != NULL) {
					uint32_t q;

					txt[n++] = '|';
					for (q = 0; q < m->ool[k].size && n < 160; q++)
						txt[n++] = (m->ool[k].data[q] >= 32 && m->ool[k].data[q] < 127) ? (char)m->ool[k].data[q] : '.';
				}
			txt[n] = 0;
			printf("darwinator: %s[%d]: IOKit request %d (size %u, %d ool): %s\n", td->td_proc->p_comm, td->td_proc->p_pid, id, m->size, m->nool, txt);
		}
		io_serve(t, dest, m, id);
		return;
	}
	if (dest->kind != KP_TASK && !(dest->kind == KP_HOST && id == 206)) {
		mig_reply(t, m, id, 0x10000 /* MIG_BAD_ID-like */ - 303, NULL, NULL, 0, NULL, 0);
		return;
	}
	switch (id) {
	case 3825:				/* mach_make_memory_entry_64(task, *size, offset, permission, *object_handle, parent): a port for the pages of the task from the address `offset` */
	case 4817:				/* mach_make_memory_entry (the same, in mach_vm.defs) */
		{
			struct proc *tp = dest->kind == KP_TASK && dest->kp != NULL ? dest->kp : td->td_proc;
			uint64_t size = m->size >= 56 ? *(uint64_t *)(d + 48) : 0, off = m->size >= 64 ? *(uint64_t *)(d + 56) : 0;
			int perm = m->size >= 68 ? (int)rd32(d, 64) : 0;
			struct kport *ep;
			struct vm_map *map;
			vm_map_entry_t e;
			struct kmsg *r;
			uint8_t *rd;

			if (m->size < 68 || size == 0 || m->reply.port == NULL || m->reply.port->dead) {
				mig_reply(t, m, id, KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0);
				break;
			}
			if (tp->p_vmspace == NULL) { mig_reply(t, m, id, KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0); break; }
			map = &tp->p_vmspace->vm_map;
			vm_map_lock_read(map);
			if (!vm_map_lookup_entry(map, (vm_offset_t)off, &e) || (e->eflags & MAP_ENTRY_IS_SUB_MAP) != 0 || e->object.vm_object == NULL ||
			    off + size > e->end) {
				vm_map_unlock_read(map);
				mig_reply(t, m, id, KERN_INVALID_ADDRESS, NULL, NULL, 0, NULL, 0);
				break;
			}
			ep = kp_new(KP_MEMENT);
			ep->mobj = e->object.vm_object;
			vm_object_reference(ep->mobj);
			ep->moff = e->offset + (off - e->start);
			ep->msize = size;
			ep->mprot = (int)(e->protection & VM_PROT_ALL);
			vm_map_unlock_read(map);
			(void)perm;
			/* the reply: a complex message with the port (a send right) and the size */
			r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->data = rd = malloc(56, M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->size = 56;
			wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
			wr32(rd, 4, 56); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
			r->carry[0].port = ep; r->carry[0].disp = MM_MOVE_SEND; r->carry[0].nameoff = 28;
			r->ncarry = 1;
			rd[28 + 10] = MM_MOVE_SEND; rd[28 + 11] = 0;
			wr32(rd, 44, 1);					/* NDR */
			*(uint64_t *)(rd + 48) = size;
			TAILQ_INSERT_TAIL(&m->reply.port->q, r, link);
			m->reply.port->qn++; kq_post(m->reply.port);
			wakeup(m->reply.port);
		}
		break;
	case 4811:				/* mach_vm_map(task, *address, size, mask, flags, object, offset, copy, cur, max, inherit) with an object: the pages of a memory entry mapped into the task */
		{
			struct proc *tp = dest->kind == KP_TASK && dest->kp != NULL ? dest->kp : td->td_proc;
			struct kport *op = m->ncarry == 1 ? m->carry[0].port : NULL;
			uint64_t addr = m->size >= 56 ? *(uint64_t *)(d + 48) : 0, size = m->size >= 64 ? *(uint64_t *)(d + 56) : 0;
			uint32_t flags = m->size >= 76 ? rd32(d, 72) : 0;
			uint64_t offset = m->size >= 84 ? *(uint64_t *)(d + 76) : 0;
			int copy = m->size >= 88 ? (int)rd32(d, 84) : 0;
			int cur = m->size >= 92 ? (int)rd32(d, 88) : 0, max = m->size >= 96 ? (int)rd32(d, 92) : 0;
			vm_offset_t va = (vm_offset_t)addr;
			struct vm_map *map;
			int rv;

			if (m->size < 100 || op == NULL || op->kind != KP_MEMENT || op->mobj == NULL || size == 0 || offset + size > op->msize || tp->p_vmspace == NULL) {
				mig_reply(t, m, id, KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0);
				break;
			}
			map = &tp->p_vmspace->vm_map;
			vm_object_reference(op->mobj);				/* the mapping keeps its own reference */
			if ((flags & 1) != 0)					/* VM_FLAGS_ANYWHERE */
				va = 0;
			rv = vm_map_find(map, op->mobj, op->moff + offset, &va, round_page(size), 0, (flags & 1) != 0 ? VMFS_ANY_SPACE : VMFS_NO_SPACE,
			    (vm_prot_t)(cur & VM_PROT_ALL), (vm_prot_t)((max | cur) & VM_PROT_ALL), copy ? MAP_COPY_ON_WRITE : MAP_INHERIT_SHARE);
			if (rv != KERN_SUCCESS) {
				vm_object_deallocate(op->mobj);
				mig_reply(t, m, id, rv == KERN_NO_SPACE ? 3 : KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0);
				break;
			}
			{
				uint64_t out64 = va;

				mig_reply_data(m, id, 0, &out64, 8);
			}
		}
		break;
	case 3409:				/* task_get_special_port(task, which): the reply carries a send right */
		which = m->size >= 36 ? rd32(d, 32) : 99;
		if (which == 4 && boot_global != NULL && !boot_global->dead && (t->special[4] == NULL || t->special[4]->kind == KP_BOOTSTRAP || t->special[4]->dead)) {
			if (t->special[4] != NULL) kp_unref(t->special[4]);
			t->special[4] = boot_global; boot_global->refs++;
		}
		if (which == 4 && t->special[4] == NULL)		/* the bootstrap port: nobody serves it, but libxpc needs one to exist (a real launchd is a program of user space) */
			t->special[4] = kp_new(KP_BOOTSTRAP);
		if (which >= 1 && which <= 15 && (which > 7 || t->special[which] == NULL)) {	/* a valid special port that is not set: success, with a null port */
			if (m->reply.port != NULL && !m->reply.port->dead) {
				struct kmsg *r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
				uint8_t *rd = r->data = malloc(40, M_DARWIN_IPC, M_WAITOK | M_ZERO);

				r->size = 40;
				wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
				wr32(rd, 4, 40); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
				rd[28 + 10] = MM_MOVE_SEND; rd[28 + 11] = 0;
				TAILQ_INSERT_TAIL(&m->reply.port->q, r, link);
				m->reply.port->qn++; kq_post(m->reply.port);
				wakeup(m->reply.port);
			}
			break;
		}
		if (which < 1 || which > 7 || t->special[which] == NULL) { mig_reply(t, m, id, which > 7 || which < 1 ? KERN_INVALID_ARGUMENT : KERN_FAILURE, NULL, NULL, 0, NULL, 0); break; }
		mig_reply(t, m, id, 0, t->special[which], NULL, 0, NULL, 0);
		break;
	case 3410:				/* task_set_special_port(task, which, port): the request carries the right */
		which = m->size >= 52 ? rd32(d, 48) : 99;
		if (m->ncarry != 1 || which < 1 || which > 7) { mig_reply(t, m, id, KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0); break; }
		if (which == 4 && m->carry[0].port != NULL && m->carry[0].port->kind == KP_PORT && td->td_ucred->cr_uid == 0 && (boot_global == NULL || boot_global->dead)) {
			if (boot_global != NULL) kp_unref(boot_global);
			boot_global = m->carry[0].port; boot_global->refs++;
			launchd_pid = td->td_proc->p_pid;
			if (darwin_debug >= 1) printf("darwinator: %s[%d] is now the launchd (bootstrap port)\n", td->td_proc->p_comm, td->td_proc->p_pid);
		}
		if (t->special[which] != NULL) kp_unref(t->special[which]);
		t->special[which] = m->carry[0].port; m->carry[0].port = NULL; m->ncarry = 0;	/* the right moves into the slot */
		mig_reply(t, m, id, 0, NULL, NULL, 0, NULL, 0);
		break;
	case 3213:				/* mach_port_request_notification(task, name, variant, sync, notify): only the dead-name variant (72) is acted on; the reply has no previous port */
		{
			struct kent *e = m->size >= 60 ? ent_by_name(t, rd32(m->data, 48)) : NULL;
			uint32_t variant = m->size >= 60 ? rd32(m->data, 52) : 0;

			if (e == NULL) { mig_reply(t, m, id, KERN_INVALID_NAME, NULL, NULL, 0, NULL, 0); break; }
			if (variant == 70 && m->ncarry == 1 && m->carry[0].port != NULL && e->port->ns_port == NULL && !e->port->dead) {
				int k;

				for (k = 0; k < NS_N; k++)
					if (ns_list[k] == NULL) {
						ns_list[k] = e->port; e->port->refs++;
						e->port->ns_port = m->carry[0].port; m->carry[0].port->refs++;
						break;
					}
				ns_check();			/* no sender yet: it is sent now */
			}
			if (variant == 72 && m->ncarry == 1 && m->carry[0].port != NULL) {
				struct kport *tp = e->port;

				if (tp->dead && !e->recv)
					notify_dead_name(m->carry[0].port, e->name);
				else if (tp->dn_port == NULL) {
					tp->dn_port = m->carry[0].port; m->carry[0].port->refs++;
					tp->dn_name = e->name;
				}
			}
			if (m->reply.port != NULL && !m->reply.port->dead) {		/* a complex reply with a null port descriptor */
				struct kmsg *r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
				uint8_t *rd = r->data = malloc(40, M_DARWIN_IPC, M_WAITOK | M_ZERO);

				r->size = 40;
				wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
				wr32(rd, 4, 40); wr32(rd, 20, id + 100); wr32(rd, 24, 1);
				rd[28 + 10] = MM_MOVE_SEND_ONCE; rd[28 + 11] = 0;
				TAILQ_INSERT_TAIL(&m->reply.port->q, r, link);
				m->reply.port->qn++; kq_post(m->reply.port);
				wakeup(m->reply.port);
			}
		}
		break;
	case 3200:				/* mach_port_names(task, *names[], *namesCnt, *types[], *typesCnt): two out-of-line arrays */
		{
			struct kmsg *r;
			struct kport *rp = m->reply.port;
			uint8_t *rd;
			uint32_t *nm, *ty, n = 0;
			int i;

			if (rp == NULL || rp->dead)
				break;
			nm = malloc(KE_N * 4, M_DARWIN_IPC, M_WAITOK | M_ZERO);
			ty = malloc(KE_N * 4, M_DARWIN_IPC, M_WAITOK | M_ZERO);
			for (i = 0; i < KE_N; i++)
				if (t->e[i].used) { nm[n] = t->e[i].name; ty[n] = entry_type(&t->e[i]); n++; }
			r = malloc(sizeof(*r), M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->data = rd = malloc(76, M_DARWIN_IPC, M_WAITOK | M_ZERO);
			r->size = 76;
			wr32(rd, 0, MACH_MSGH_BITS_COMPLEX | MM_MOVE_SEND_ONCE);
			wr32(rd, 4, 76); wr32(rd, 20, id + 100); wr32(rd, 24, 2);
			r->ool[0].type = 1; r->ool[0].off = 28; r->ool[0].size = n * 4; r->ool[0].data = (uint8_t *)nm;
			r->ool[1].type = 1; r->ool[1].off = 44; r->ool[1].size = n * 4; r->ool[1].data = (uint8_t *)ty;
			r->nool = 2;
			wr32(rd, 28 + 12, n * 4); rd[28 + 11] = 1;
			wr32(rd, 44 + 12, n * 4); rd[44 + 11] = 1;
			wr32(rd, 68, n); wr32(rd, 72, n);
			TAILQ_INSERT_TAIL(&rp->q, r, link);
			rp->qn++; kq_post(rp);
			wakeup(rp);
		}
		break;
	case 206:				/* host_get_clock_service(host, clock_id (0 system, 1 calendar), *clock): the reply carries a send right to a clock port */
		{
			struct kport *cp = kp_new(KP_CLOCK);

			cp->kpid = m->size >= 36 ? (rd32(m->data, 32) == 1 ? 1 : 0) : 0;
			mig_reply(t, m, id, 0, cp, NULL, 0, NULL, 0);
			kp_unref(cp);
		}
		break;
	case 3402:				/* task_threads(task, *threads[], *count): a port for every thread */
		{
			struct kport *arr[64];
			lwpid_t tids[64];
			struct proc *tp = dest->kind == KP_TASK && dest->kp != NULL ? dest->kp : td->td_proc;
			struct thread *tdx;
			uint32_t n = 0;
			int i;

			PROC_LOCK(tp);
			FOREACH_THREAD_IN_PROC(tp, tdx) {
				if (n < 64)
					tids[n++] = tdx->td_tid;
			}
			PROC_UNLOCK(tp);
			for (i = 0; i < (int)n; i++) {
				arr[i] = kp_new(KP_THREAD);
				arr[i]->kp = tp; arr[i]->kpid = tids[i];
			}
			mig_reply(t, m, id, 0, NULL, NULL, 0, arr, n);
			for (i = 0; i < (int)n; i++)
				kp_unref(arr[i]);
		}
		break;
	case 3403:				/* mach_ports_register(task, ports[], count): up to 3 ports that the children get */
		if (m->nool != 1 || m->ool[0].type != 2 || m->ool[0].count > 3) { mig_reply(t, m, id, KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0); break; }
		{
			uint32_t j;

			for (j = 0; j < 3; j++) { if (t->reg[j] != NULL) kp_unref(t->reg[j]); t->reg[j] = j < m->ool[0].count ? m->ool[0].ports[j] : NULL; if (j < m->ool[0].count) m->ool[0].ports[j] = NULL; }
		}
		mig_reply(t, m, id, 0, NULL, NULL, 0, NULL, 0);
		break;
	case 3404:				/* mach_ports_lookup(task): the registered ports, as an array in the caller */
		{
			uint32_t j, n = 0;
			struct kport *arr[3];

			for (j = 0; j < 3; j++) if (t->reg[j] != NULL) arr[n++] = t->reg[j];
			mig_reply(t, m, id, 0, NULL, NULL, 0, arr, n);
		}
		break;
	case 3418:				/* semaphore_create(task, policy, value): the reply carries a send right to a new semaphore */
		{
			struct kport *sp = kp_new(KP_SEM);

			sp->count = m->size >= 40 ? (int)rd32(d, 36) : 0;
			mig_reply(t, m, id, 0, sp, NULL, 0, NULL, 0);
			kp_unref(sp);
		}
		break;
	case 3419:				/* semaphore_destroy(task, semaphore): the semaphore comes as a port descriptor */
		if (m->ncarry == 1 && m->carry[0].port != NULL && m->carry[0].port->kind == KP_SEM) {
			m->carry[0].port->dead = true;
			wakeup(m->carry[0].port);
			mig_reply(t, m, id, 0, NULL, NULL, 0, NULL, 0);
		} else
			mig_reply(t, m, id, KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0);
		break;
	default:
		mig_reply(t, m, id, 0x10000 - 303, NULL, NULL, 0, NULL, 0);	/* MIG_BAD_ID */
		break;
	}
}

/* ---- fork and exit ---- */

static void
darwin_mach_fork(void *arg __unused, struct proc *p1, struct proc *p2, int flags __unused)
{
	struct ktask *t1, *t2;
	int i;

	if (p1->p_sysent != &darwin_sysvec)
		return;
	sx_xlock(&km_sx);
	t1 = kt_get(p1, false);
	if (t1 != NULL && (t2 = kt_task(p2)) != NULL) {
		for (i = 0; i < 8; i++)
			if (t1->special[i] != NULL) { t2->special[i] = t1->special[i]; t1->special[i]->refs++; }
		for (i = 0; i < 3; i++)
			if (t1->reg[i] != NULL) { t2->reg[i] = t1->reg[i]; t1->reg[i]->refs++; }
	}
	sx_xunlock(&km_sx);
}

static void
darwin_mach_exit(void *arg __unused, struct proc *p)
{
	struct ktask *t;
	int i;

	if (darwin_debug >= 1 && p->p_sysent == &darwin_sysvec && p->p_xsig == 0)
		printf("darwinator: %s[%d] exited with status %d\n", p->p_comm, p->p_pid, (int)p->p_xexit);
	if (darwin_debug >= 1 && p->p_sysent == &darwin_sysvec && (p->p_xsig == SIGILL || p->p_xsig == SIGSEGV || p->p_xsig == SIGBUS || p->p_xsig == SIGABRT || p->p_xsig == SIGTRAP)) {
		struct thread *tdp = curthread;

		/* a Darwin process that dies of a signal of a fault: where (the thread that exits is the one that took it) */
		uint64_t w[40];
		int wi;

		printf("darwinator: %s[%d] died of signal %d at %#lx (stack %#lx, rax %#lx)\n", p->p_comm, p->p_pid, p->p_xsig, (u_long)tdp->td_frame->tf_rip, (u_long)tdp->td_frame->tf_rsp, (u_long)tdp->td_frame->tf_rax);
		if (tdp == curthread) {			/* the frames (rbp chain): the return addresses of the callers */
			uint64_t fp = tdp->td_frame->tf_rbp, pair[2];
			int fi;

			for (fi = 0; fi < 24 && fp != 0 && copyin((void *)fp, pair, sizeof(pair)) == 0; fi++) {
				printf("darwinator:   frame %d: return %#lx\n", fi, (u_long)pair[1]);
				if (pair[0] <= fp) break;
				fp = pair[0];
			}
		}
		if (tdp == curthread) {			/* a few words of the stack: the return addresses show who called */
			uint64_t sw[12];
			int k;

			if (copyin((void *)tdp->td_frame->tf_rsp, sw, sizeof(sw)) == 0)
				for (k = 0; k < 12; k++)
					printf("darwinator:   [rsp+%d] %#lx\n", k * 8, (u_long)sw[k]);
		}
		if (copyin((void *)(uintptr_t)tdp->td_frame->tf_rsp, w, sizeof(w)) == 0)
			for (wi = 0; wi < 40; wi++)
				if (w[wi] >= 0x7ff800000000ULL && w[wi] < 0x7fff00000000ULL)
					printf("darwinator:   stack[%d] = %#lx\n", wi, (u_long)w[wi]);
	}

	sx_xlock(&km_sx);
	if ((t = kt_get(p, false)) != NULL) {
		for (i = 0; i < KE_N; i++)
			if (t->e[i].used) {
				if (t->e[i].recv) port_die(t->e[i].port);
				kp_unref(t->e[i].port);
				t->e[i].used = false;
			}
		for (i = 0; i < 8; i++) { kp_unref(t->special[i]); t->special[i] = NULL; }
		for (i = 0; i < 3; i++) { kp_unref(t->reg[i]); t->reg[i] = NULL; }
		t->used = false;
		ns_check();
	}
	sx_xunlock(&km_sx);
	darwin_kq_exit(p);
	darwin_wq_exit(p);
	darwin_pth_exit(p);
}

static eventhandler_tag fork_tag, exit_tag;

void
darwin_mach_init(void)
{
	(void)kqueue_del_filteropts(EVFILT_MACHPORT_SLOT);
	if (kqueue_add_filteropts(EVFILT_MACHPORT_SLOT, &mp_filterops) != 0)
		printf("darwinator: EVFILT_MACHPORT is not available\n");
	fork_tag = EVENTHANDLER_REGISTER(process_fork, darwin_mach_fork, NULL, EVENTHANDLER_PRI_ANY);
	exit_tag = EVENTHANDLER_REGISTER(process_exit, darwin_mach_exit, NULL, EVENTHANDLER_PRI_ANY);
}

void
darwin_mach_fini(void)
{
	int i;

	(void)kqueue_del_filteropts(EVFILT_MACHPORT_SLOT);
	EVENTHANDLER_DEREGISTER(process_fork, fork_tag);
	EVENTHANDLER_DEREGISTER(process_exit, exit_tag);
	for (i = 0; i < KT_N; i++)
		if (kt_tab[i] != NULL) { free(kt_tab[i], M_DARWIN_IPC); kt_tab[i] = NULL; }
}

/* _kernelrpc_mach_port_request_notification_trap (77): (task, name, variant, sync, notify, notifyPoly, *previous): the same as the MIG routine 3213 (the dead-name and no-senders variants), with the notify port given by name */
static int
darwin_port_request_notification_trap(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct kent *e, *ne;
	uint32_t variant = (uint32_t)uap->a[2];
	int kr = KERN_SUCCESS;
	uint32_t prev = 0;

	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL || (e = ent_by_name(t, (uint32_t)uap->a[1])) == NULL) {
		sx_xunlock(&km_sx);
		return (kr_ret(td, KERN_INVALID_NAME));
	}
	ne = ent_by_name(t, (uint32_t)uap->a[4]);
	if (ne != NULL) {
		if (variant == 70 && e->port->ns_port == NULL && !e->port->dead) {
			int k;

			for (k = 0; k < NS_N; k++)
				if (ns_list[k] == NULL) {
					ns_list[k] = e->port; e->port->refs++;
					e->port->ns_port = ne->port; ne->port->refs++;
					break;
				}
			ns_check();
		}
		if (variant == 72) {
			struct kport *tp = e->port;

			if (tp->dead && !e->recv)
				notify_dead_name(ne->port, e->name);
			else if (tp->dn_port == NULL) {
				tp->dn_port = ne->port; ne->port->refs++;
				tp->dn_name = e->name;
			}
		}
	}
	sx_xunlock(&km_sx);
	if (uap->a[6] != 0 && copyout(&prev, (void *)uap->a[6], 4) != 0)
		kr = KERN_INVALID_ADDRESS;
	return (kr_ret(td, kr));
}

/* work_interval_ctl(operation, work_interval_id, arg, len) (499): the work intervals of the scheduler (the window server makes one for its rendering).  CREATE (1) and CREATE2 (2) give an id and a port (the
 * parameters are {uint64 id, uint32 port, uint32 flags}); the other operations (join, leave, notify, destroy) succeed: there is no deadline scheduling here */
int
darwin_work_interval_ctl(struct thread *td, struct darwin_args *uap)
{
	static uint64_t next_id = 1;
	uint32_t op = (uint32_t)uap->a[0];

	td->td_retval[0] = 0;
	if (op == 1 || op == 2) {
		struct ktask *t;
		struct kport *wp;
		struct kent *e;
		uint8_t params[16];
		uint32_t name = 0;
		int error;

		if (uap->a[2] == 0 || uap->a[3] < sizeof(params))
			return (EINVAL);
		sx_xlock(&km_sx);
		if ((t = kt_task(td->td_proc)) != NULL) {
			wp = kp_new(KP_PORT);
			e = ent_add(t, wp, MM_MOVE_SEND);
			wp->refs--;
			if (e != NULL) name = e->name; else kp_unref(wp);
		}
		sx_xunlock(&km_sx);
		memset(params, 0, sizeof(params));
		next_id++;
		memcpy(params, &next_id, 8);
		memcpy(params + 8, &name, 4);
		error = copyout(params, (void *)uap->a[2], sizeof(params));
		if (darwin_debug >= 1)
			printf("darwinator: %s[%d]: work interval %lu made (port %#x)\n", td->td_proc->p_comm, td->td_proc->p_pid, (u_long)next_id, name);
		return (error);
	}
	return (0);
}

/* _kernelrpc_mach_port_get_attributes_trap (40): (task, name, flavor, *info, *count), the same answers as the MIG routine 3217 (limits, receive status, dnrequests size) */
static int
darwin_port_get_attributes_trap(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct kent *e;
	uint32_t flavor = (uint32_t)uap->a[2], out[10], n = 0, cnt;
	int kr = KERN_SUCCESS;

	memset(out, 0, sizeof(out));
	sx_xlock(&km_sx);
	if ((t = kt_task(td->td_proc)) == NULL || (e = ent_by_name(t, (uint32_t)uap->a[1])) == NULL)
		kr = KERN_INVALID_NAME;
	else if (flavor == 1) { out[0] = 5; n = 1; }
	else if (flavor == 2) { out[1] = e->port->set != NULL ? 1 : 0; out[4] = 5; out[5] = (uint32_t)e->port->qn; n = 10; }
	else if (flavor == 3) { n = 1; }
	else kr = KERN_INVALID_ARGUMENT;
	sx_xunlock(&km_sx);
	if (kr == KERN_SUCCESS) {
		if (copyin((void *)uap->a[4], &cnt, 4) != 0 || cnt < n)
			return (kr_ret(td, KERN_INVALID_ARGUMENT));
		if (copyout(out, (void *)uap->a[3], n * 4) != 0 || copyout(&n, (void *)uap->a[4], 4) != 0)
			kr = KERN_INVALID_ADDRESS;
	}
	return (kr_ret(td, kr));
}

/* task_name_for_pid (44) and task_for_pid (45)(target, pid, *task): a send right to the task port of the process (the task itself for the own pid); pid_for_task (46)(task, *pid) */
static int
darwin_task_for_pid_trap(struct thread *td, struct darwin_args *uap)
{
	struct ktask *t;
	struct proc *tp;
	struct kport *tport;
	struct kent *e;
	uint32_t name = 0;
	pid_t pid = (pid_t)uap->a[1];
	int kr = KERN_SUCCESS;

	if (pid == td->td_proc->p_pid) {
		name = NAME_TASK_SELF;
	} else {
		if ((tp = pfind(pid)) == NULL)
			return (kr_ret(td, 5 /* KERN_FAILURE */));
		PROC_UNLOCK(tp);
		sx_xlock(&km_sx);
		if ((t = kt_task(td->td_proc)) == NULL) kr = KERN_NO_SPACE;
		else {
			tport = kp_new(KP_TASK); tport->kp = tp; tport->kpid = pid;
			e = ent_add(t, tport, MM_MAKE_SEND);
			tport->refs--;
			if (e != NULL) name = e->name; else { kp_unref(tport); kr = KERN_NO_SPACE; }
		}
		sx_xunlock(&km_sx);
	}
	if (kr == KERN_SUCCESS && copyout(&name, (void *)uap->a[2], 4) != 0)
		kr = KERN_INVALID_ADDRESS;
	return (kr_ret(td, kr));
}

static const struct {
	int		num, nargs;
	sy_call_t	*fn;
	const char	*name;
} darwin_ipc[] = {
#define	E(num, nargs, fn, name)	{ DARWIN_MACH_BASE + (num), (nargs), (sy_call_t *)(fn), (name) }
	E(16, 3, darwin_port_allocate,		"_kernelrpc_mach_port_allocate_trap"),
	E(18, 2, darwin_port_deallocate,	"_kernelrpc_mach_port_deallocate_trap"),
	E(19, 4, darwin_port_mod_refs,		"_kernelrpc_mach_port_mod_refs_trap"),
	E(20, 3, darwin_port_move_member,	"_kernelrpc_mach_port_move_member_trap"),
	E(22, 3, darwin_port_move_member,	"_kernelrpc_mach_port_insert_member_trap"),
	E(23, 3, darwin_port_extract_member,	"_kernelrpc_mach_port_extract_member_trap"),
	E(40, 5, darwin_port_get_attributes_trap,	"_kernelrpc_mach_port_get_attributes_trap"),
	E(41, 4, darwin_port_guard,		"_kernelrpc_mach_port_guard_trap"),
	E(42, 3, darwin_port_unguard,		"_kernelrpc_mach_port_unguard_trap"),
	E(43, 3, darwin_generate_activity_id,	"mach_generate_activity_id"),
	E(44, 3, darwin_task_for_pid_trap,	"task_name_for_pid"),
	E(45, 3, darwin_task_for_pid_trap,	"task_for_pid"),
	E(76, 3, darwin_port_type_trap,		"_kernelrpc_mach_port_type_trap"),
	E(77, 7, darwin_port_request_notification_trap,	"_kernelrpc_mach_port_request_notification_trap"),
	E(91, 0, darwin_mk_timer_create,	"mk_timer_create_trap"),
	E(92, 1, darwin_mk_timer_destroy,	"mk_timer_destroy_trap"),
	E(93, 2, darwin_mk_timer_arm,		"mk_timer_arm_trap"),
	E(94, 2, darwin_mk_timer_cancel,	"mk_timer_cancel_trap"),
	E(95, 4, darwin_mk_timer_arm_leeway,	"mk_timer_arm_leeway_trap"),
	E(21, 4, darwin_port_insert_right,	"_kernelrpc_mach_port_insert_right_trap"),
	E(33, 1, darwin_sem_signal,		"semaphore_signal_trap"),
	E(34, 1, darwin_sem_signal_all,		"semaphore_signal_all_trap"),
	E(36, 1, darwin_sem_wait,		"semaphore_wait_trap"),
	E(37, 2, darwin_sem_wait_signal,	"semaphore_wait_signal_trap"),
	E(38, 3, darwin_sem_timedwait,		"semaphore_timedwait_trap"),
	E(39, 4, darwin_sem_timedwait_signal,	"semaphore_timedwait_signal_trap"),
	E(24, 4, darwin_port_construct,		"_kernelrpc_mach_port_construct_trap"),
	E(25, 4, darwin_port_destruct,		"_kernelrpc_mach_port_destruct_trap"),
	E(26, 0, darwin_mach_reply_port,	"mach_reply_port"),
	E(27, 0, darwin_self_thread,		"thread_self_trap"),
	E(28, 0, darwin_self_task,		"task_self_trap"),
	E(29, 0, darwin_self_host,		"host_self_trap"),
	E(31, 7, darwin_mach_msg,		"mach_msg_trap"),
	E(47, 8, darwin_mach_msg2,		"mach_msg2_trap"),
	E(50, 0, darwin_thread_special_reply_port, "thread_get_special_reply_port"),
	E(70, 4, darwin_host_create_voucher,	"host_create_mach_voucher_trap"),
	E(32, 9, darwin_mach_msg,		"mach_msg_overwrite_trap"),
#undef E
};

void
darwin_ipc_syscalls_init(void)
{
	u_int i;

	for (i = 0; i < nitems(darwin_ipc); i++) {
		int n = darwin_ipc[i].num;

		darwin_sysent[n] = (struct sysent){ .sy_narg = darwin_ipc[i].nargs, .sy_call = darwin_ipc[i].fn, .sy_auevent = AUE_NULL, .sy_thrcnt = SY_THR_STATIC };
		darwin_syscallnames[n] = darwin_ipc[i].name;
	}
}
