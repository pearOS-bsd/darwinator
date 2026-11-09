/* probe2: behaviour of the real macOS for posix_spawn, named semaphores, access_extended and statfs: compare with tests/c/spawn.c and syscalls.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
extern char **environ;
#define OUT "/tmp/macprobe2.out"

static void slurp(const char *p, char *b, int n) { int fd = open(p, O_RDONLY); int r = fd < 0 ? 0 : (int)read(fd, b, n - 1); if (fd >= 0) close(fd); b[r > 0 ? r : 0] = 0; }

static void spawn_tests(void)
{
	char buf[512]; pid_t pid = -5; int st = 0, r;
	posix_spawn_file_actions_t fa; posix_spawnattr_t at;
	printf("sizeof(posix_spawnattr_t) %zu  sizeof(posix_spawn_file_actions_t) %zu\n", sizeof(posix_spawnattr_t), sizeof(posix_spawn_file_actions_t));
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	char *av[] = { "echo", "hello", "spawn", 0 };
	r = posix_spawn(&pid, "/bin/echo", &fa, NULL, av, environ);
	waitpid(pid, &st, 0); slurp(OUT, buf, sizeof buf);
	printf("spawn echo: r=%d pid>1=%d status=%d out=[%s]\n", r, pid > 1, st, buf);
	pid = -5; char *av2[] = { "nothere", 0 };
	r = posix_spawn(&pid, "/no/such/program", NULL, NULL, av2, environ);
	printf("spawn missing: r=%d pid=%d\n", r, pid);
	posix_spawn_file_actions_destroy(&fa); posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 9, OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	posix_spawn_file_actions_adddup2(&fa, 9, 1); posix_spawn_file_actions_addclose(&fa, 9); posix_spawn_file_actions_addchdir_np(&fa, "/usr");
	char *av3[] = { "pwd", 0 };
	r = posix_spawn(&pid, "/bin/pwd", &fa, NULL, av3, environ); waitpid(pid, &st, 0); slurp(OUT, buf, sizeof buf);
	printf("spawn chdir+dup2: r=%d out=[%s]\n", r, buf);
	posix_spawnattr_init(&at); posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
	posix_spawnattr_setpgroup(&at, 0); sigset_t m; sigemptyset(&m); sigaddset(&m, SIGINT); posix_spawnattr_setsigmask(&at, &m);
	posix_spawn_file_actions_destroy(&fa); posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	char *av4[] = { "sh", "-c", "echo $$ $(ps -o pgid= -p $$ | tr -d ' ')", 0 };
	r = posix_spawn(&pid, "/bin/sh", &fa, &at, av4, environ); waitpid(pid, &st, 0); slurp(OUT, buf, sizeof buf);
	printf("spawn SETPGROUP: r=%d pid=%d out=[%s]\n", r, pid, buf);
	char *ev[] = { "FOO=bar", "SPAWN=1", 0 }; char *av5[] = { "env", 0 };
	r = posix_spawn(&pid, "/usr/bin/env", &fa, NULL, av5, ev); waitpid(pid, &st, 0); slurp(OUT, buf, sizeof buf);
	printf("spawn env: r=%d out=[%s]\n", r, buf);
	char *av6[] = { "sh", "-c", "exit 7", 0 };
	r = posix_spawn(&pid, "/bin/sh", NULL, NULL, av6, environ); waitpid(pid, &st, 0);
	printf("spawn exit 7: r=%d status=%d\n", r, st);
	unlink(OUT);
}

static void sem_tests(void)
{
	sem_unlink("/macprobe-sem");
	sem_t *s = sem_open("/macprobe-sem", O_CREAT, 0600, 2);
	printf("sem_open: %s  value-as-int=%ld  errno=%d\n", s == SEM_FAILED ? "SEM_FAILED" : "ok", (long)(intptr_t)s, errno);
	if (s == SEM_FAILED) return;
	printf("wait %d  trywait %d  ", sem_wait(s), sem_trywait(s));
	int r = sem_trywait(s); printf("trywait at zero %d errno %d  post %d  trywait %d  close %d\n", r, errno, sem_post(s), sem_trywait(s), sem_close(s));
	printf("unlink %d\n", sem_unlink("/macprobe-sem"));
	s = sem_open("/macprobe-sem", 0); printf("open without O_CREAT: %s errno %d\n", s == SEM_FAILED ? "SEM_FAILED" : "ok", errno);
}

struct ad { unsigned off; int flags; int pad[2]; };
static void access_tests(void)
{
	/* raw access_extended (284): [R_OK|W_OK|X_OK values] and the _READ_OK style bits of <sys/unistd.h> */
	int flagsets[] = { R_OK, W_OK, X_OK, F_OK, 1 << 9, 1 << 10, 1 << 11 };
	for (unsigned k = 0; k < sizeof flagsets / sizeof *flagsets; k++) {
		char blob[256]; memset(blob, 0, sizeof blob);
		struct ad e[2] = { { 32, flagsets[k], { 0, 0 } }, { 40, flagsets[k], { 0, 0 } } };
		memcpy(blob, e, 32); strcpy(blob + 32, "/"); strcpy(blob + 40, "/nonexistent");
		int res[2] = { 99, 99 };
		errno = 0;
		int r = (int)syscall(SYS_access_extended, blob, (size_t)64, res, (uid_t)-1);
		printf("access_extended flags=0x%x: r=%d errno=%d res=[%d,%d]\n", flagsets[k], r, errno, res[0], res[1]);
	}
}

static void statfs_tests(void)
{
	struct statfs a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
	int r = statfs("/", &a); int fd = open("/", O_RDONLY); int r2 = fstatfs(fd, &b);
	printf("sizeof(struct statfs) %zu  statfs %d fstatfs %d  bsize %u blocks>0 %d type=%s on=%s\n", sizeof a, r, r2, a.f_bsize, a.f_blocks > 0, a.f_fstypename, a.f_mntonname);
	long raw = syscall(157, "/", &b); printf("raw syscall 157 (statfs): %ld, same f_bsize as libc: %d\n", raw, raw == 0 && b.f_bsize == a.f_bsize);
}

int main(void)
{
	spawn_tests(); sem_tests(); access_tests(); statfs_tests();
	return 0;
}
