/* condvar: a broadcast must wake the waiter that is already asleep, not the broadcaster that waits right after it.
 * SwiftUI's MovableLock does this: the render thread sleeps on the condition (MovableLockSyncMain), the main thread runs the pending work, broadcasts and
 * at once waits on the same condition itself (MovableLockLock); with plain wake tokens in psynch the main thread took both wakes of its own broadcast
 * and the render thread never woke (darwinator src/syscalls_sync.c).  Also pthread_cond_signal_thread_np, which names the thread to wake.
 * Exit 0 and "condvar: ok" when every round completes within its timeout. */
/* built on FreeBSD for the Mac's libSystem: the Mac's types, not the headers of the build host (FreeBSD's pthread_mutex_t is a pointer, its initializer NULL) */
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
typedef struct _opaque_pthread *pthread_t;
struct timespec { long tv_sec, tv_nsec; };
struct timeval { long tv_sec; int tv_usec; };
#define PTHREAD_MUTEX_INITIALIZER { 0x32AAABA7, { 0 } }
#define PTHREAD_COND_INITIALIZER { 0x3CB0B1BB, { 0 } }
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
pthread_t pthread_self(void);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_cond_signal_thread_np(pthread_cond_t *, pthread_t);
int gettimeofday(struct timeval *, void *);
int printf(const char *, ...);

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int pending, done, owner_waiting;
static pthread_t mainth;

static void *render(void *a)
{
	(void)a;
	for (int i = 0; i < 200; i++) {
		pthread_mutex_lock(&m);
		pending = 1;					/* work for the main thread */
		if (owner_waiting) pthread_cond_signal_thread_np(&c, mainth);
		while (pending) pthread_cond_wait(&c, &m);	/* until the main thread did it */
		pthread_mutex_unlock(&m);
	}
	pthread_mutex_lock(&m); done = 1; pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
	return 0;
}

int main(void)
{
	pthread_t t; int rounds = 0, timeouts = 0;
	mainth = pthread_self();
	pthread_create(&t, 0, render, 0);
	pthread_mutex_lock(&m);
	while (!done) {
		if (pending) { pending = 0; rounds++; pthread_cond_broadcast(&c); }	/* the work, then the wake ... */
		struct timeval tv; gettimeofday(&tv, 0); struct timespec ts = { tv.tv_sec + 3, tv.tv_usec * 1000 };	/* libpthread measures the deadline against gettimeofday */
		owner_waiting = 1;
		if (!done && !pending && pthread_cond_timedwait(&c, &m, &ts) != 0) timeouts++;	/* ... and at once the broadcaster waits on the same condition */
		owner_waiting = 0;
		if (timeouts > 2) break;
	}
	pthread_mutex_unlock(&m);
	if (timeouts > 2) { printf("condvar: FAIL after %d rounds: the waiter was not woken\n", rounds); return 1; }
	pthread_join(t, 0);
	printf("condvar: ok (%d rounds)\n", rounds);
	return 0;
}
