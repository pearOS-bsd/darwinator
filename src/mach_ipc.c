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
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/syscallsubr.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/ptrace.h>
#include <sys/vmmeter.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
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

enum { KP_PORT, KP_TASK, KP_HOST, KP_THREAD, KP_SET, KP_SEM, KP_FILE, KP_CLOCK };

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
};

struct kcarry { struct kport *port; int disp; uint32_t nameoff; };	/* a right in a message; nameoff: where its name goes in the data */
struct kool { uint8_t *data; uint32_t size; uint32_t off; int type; int disp; uint32_t count; struct kport **ports; };

struct kmsg {
	TAILQ_ENTRY(kmsg)	link;
	uint32_t		size;
	uint8_t			*data;
	struct kcarry		reply;		/* the local port of the header: the reply right */
	int			ncarry;
	struct kcarry		carry[8];
	int			nool;
	struct kool		ool[4];
};

struct kent { bool used, recv; int send, sonce; uint32_t name; struct kport *port; };
#define	KE_N	96
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
static struct sx km_sx;
SX_SYSINIT(darwin_mach, &km_sx, "darwin mach ipc");

/* ---- ports and names (the lock is held) ---- */

static struct kport *
kp_new(int kind)
{
	struct kport *p = malloc(sizeof(*p), M_DARWIN_IPC, M_WAITOK | M_ZERO);

	p->refs = 1;
	p->kind = kind;
	TAILQ_INIT(&p->q);
	return (p);
}

static void kmsg_free(struct kmsg *m);

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
			if ((uint32_t)uap->a[2] == 0) { m->port->set = NULL; kr = KERN_SUCCESS; }
			else if ((a = ent_by_name(t, (uint32_t)uap->a[2])) != NULL && a->recv && a->port->kind == KP_SET) { m->port->set = a->port; wakeup(a->port); kr = KERN_SUCCESS; }
		}
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

static uint32_t rd32(const uint8_t *b, size_t off) { uint32_t v; memcpy(&v, b + off, 4); return (v); }
static void wr32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }
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
	if ((de = ent_by_name(t, rname)) == NULL) { sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_INVALID_DEST); }
	dp = de->port;
	if (dp->dead) { sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_INVALID_DEST); }
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
	if (nd != KP_PORT) {						/* the kernel answers for the task and host ports */
		mig_server(td, t, dp, m);
		kp_unref(dp);
		sx_xunlock(&km_sx);
		kmsg_free(m);
		return (KERN_SUCCESS);
	}
	if (dp->qn >= 64) { kp_unref(dp); sx_xunlock(&km_sx); kmsg_free(m); return (MACH_SEND_NO_BUFFER); }
	TAILQ_INSERT_TAIL(&dp->q, m, link);
	dp->qn++;
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

static int
recv_msg(struct thread *td, uint64_t uaddr, uint32_t option, uint32_t rsize, uint32_t rname, uint32_t timeout_ms)
{
	struct ktask *t;
	struct kent *e;
	struct kport *rp, *srcp = NULL;
	uint32_t setname = rname;
	struct kmsg *m;
	int timo = (option & MACH_RCV_TIMEOUT) ? MAX(1, (int)((int64_t)timeout_ms * hz / 1000)) : 0, error = 0, i;
	uint8_t *d;
	uint32_t bits, ldisp0;

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
	if (m->size > rsize) {
		kp_unref(rp);
		sx_xunlock(&km_sx);
		return (MACH_RCV_TOO_LARGE);
	}
	TAILQ_REMOVE(&srcp->q, m, link);
	srcp->qn--;
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
	sx_xlock(&km_sx);
	m->nool = 0;					/* the ports of the arrays were handed out */
	kmsg_free(m);
	sx_xunlock(&km_sx);
	return (error != 0 ? KERN_INVALID_ADDRESS : KERN_SUCCESS);
}

static int darwin_mach_msg(struct thread *td, struct darwin_args *uap);

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
	rp->qn++;
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
	to->qn++;
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
	rp->qn++;
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
			m->reply.port->qn++;
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
				m->reply.port->qn++;
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
	case 3229:					/* mach_port_set_context */
	case 8000: case 8001:				/* task_restartable_ranges_register (the ranges are not used), task_restartable_ranges_synchronize: libobjc needs them to answer 0 */
	case 3407: case 3408:				/* task_suspend, task_resume */
	case 3406:					/* task_set_info */
	case 3413:					/* task_set_exception_ports: the handlers are not called here */
	case 3420: case 3421: case 3423:		/* task_policy_set/get/task_policy */
	case 3444: case 3445:				/* task_register_dyld_image_infos / unregister */
	case 3447: case 3448:				/* dyld shared cache image info / state */
		mig_reply_data(m, id, 0, NULL, 0);
		return (true);
	case 3228:					/* mach_port_get_context(task, name, *context: 8 bytes) */
		mig_reply_data(m, id, 0, out, 8);
		return (true);
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

static void
mig_server(struct thread *td, struct ktask *t, struct kport *dest, struct kmsg *m)
{
	uint8_t *d = m->data;
	int id = (int)rd32(d, 20);
	uint32_t which;

	if (mig_info(td, t, dest, m, id))
		return;
	if (dest->kind != KP_TASK && !(dest->kind == KP_HOST && id == 206)) {
		mig_reply(t, m, id, 0x10000 /* MIG_BAD_ID-like */ - 303, NULL, NULL, 0, NULL, 0);
		return;
	}
	switch (id) {
	case 3409:				/* task_get_special_port(task, which): the reply carries a send right */
		which = m->size >= 36 ? rd32(d, 32) : 99;
		if (which < 1 || which > 7 || t->special[which] == NULL) { mig_reply(t, m, id, which > 7 || which < 1 ? KERN_INVALID_ARGUMENT : KERN_FAILURE, NULL, NULL, 0, NULL, 0); break; }
		mig_reply(t, m, id, 0, t->special[which], NULL, 0, NULL, 0);
		break;
	case 3410:				/* task_set_special_port(task, which, port): the request carries the right */
		which = m->size >= 52 ? rd32(d, 48) : 99;
		if (m->ncarry != 1 || which < 1 || which > 7) { mig_reply(t, m, id, KERN_INVALID_ARGUMENT, NULL, NULL, 0, NULL, 0); break; }
		if (t->special[which] != NULL) kp_unref(t->special[which]);
		t->special[which] = m->carry[0].port; m->carry[0].port = NULL; m->ncarry = 0;	/* the right moves into the slot */
		mig_reply(t, m, id, 0, NULL, NULL, 0, NULL, 0);
		break;
	case 3213:				/* mach_port_request_notification(task, name, variant, sync, notify): only the dead-name variant (72) is acted on; the reply has no previous port */
		{
			struct kent *e = m->size >= 60 ? ent_by_name(t, rd32(m->data, 48)) : NULL;
			uint32_t variant = m->size >= 60 ? rd32(m->data, 52) : 0;

			if (e == NULL) { mig_reply(t, m, id, KERN_INVALID_NAME, NULL, NULL, 0, NULL, 0); break; }
			if (variant == 70 && m->ncarry == 1 && e->port->ns_port == NULL && !e->port->dead) {
				int k;

				for (k = 0; k < NS_N; k++)
					if (ns_list[k] == NULL) {
						ns_list[k] = e->port; e->port->refs++;
						e->port->ns_port = m->carry[0].port; m->carry[0].port->refs++;
						break;
					}
				ns_check();			/* no sender yet: it is sent now */
			}
			if (variant == 72 && m->ncarry == 1) {
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
				m->reply.port->qn++;
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
			rp->qn++;
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
}

static eventhandler_tag fork_tag, exit_tag;

void
darwin_mach_init(void)
{
	fork_tag = EVENTHANDLER_REGISTER(process_fork, darwin_mach_fork, NULL, EVENTHANDLER_PRI_ANY);
	exit_tag = EVENTHANDLER_REGISTER(process_exit, darwin_mach_exit, NULL, EVENTHANDLER_PRI_ANY);
}

void
darwin_mach_fini(void)
{
	int i;

	EVENTHANDLER_DEREGISTER(process_fork, fork_tag);
	EVENTHANDLER_DEREGISTER(process_exit, exit_tag);
	for (i = 0; i < KT_N; i++)
		if (kt_tab[i] != NULL) { free(kt_tab[i], M_DARWIN_IPC); kt_tab[i] = NULL; }
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
