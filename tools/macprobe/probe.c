/* probe.c: prints what the real macOS gives for the things darwinator had to assume (sizes, constants, sysctl values, the result of some calls).  Build: cc -o probe probe.c   Run: ./probe > probe.txt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <dirent.h>
#include <fts.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/attr.h>
#include <sys/ucontext.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <netinet/in.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>


#define SZ(t) printf("sizeof %s = %zu\n", #t, sizeof(t))
#define OFF(t, f) printf("offsetof %s.%s = %zu\n", #t, #f, __builtin_offsetof(t, f))
#define CONST(c) printf("%s = %ld\n", #c, (long)(c))

static void on_usr1(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = uc;
	printf("signal handler: sig %d si_signo %d uc_mcsize %zu uc_onstack %d uc_sigmask %u\n", sig, si->si_signo, (size_t)u->uc_mcsize, u->uc_onstack, (unsigned)u->uc_sigmask);
	printf("signal handler: uc_mcontext - uc = %ld (mcontext64 follows the ucontext when 56)\n", (long)((char *)u->uc_mcontext - (char *)u));
#if defined(__x86_64__)
	printf("signal handler: ss.rip %#llx ss.rsp %#llx\n", (unsigned long long)u->uc_mcontext->__ss.__rip, (unsigned long long)u->uc_mcontext->__ss.__rsp);
#else
	printf("signal handler: ss.pc %#llx ss.sp %#llx\n", (unsigned long long)__darwin_arm_thread_state64_get_pc(u->uc_mcontext->__ss), (unsigned long long)__darwin_arm_thread_state64_get_sp(u->uc_mcontext->__ss));
#endif
}

int main(void)
{
	printf("== machine\n");
	{ char b[256]; size_t n = sizeof(b); if (sysctlbyname("hw.machine", b, &n, 0, 0) == 0) printf("hw.machine = %s\n", b); n = sizeof(b); if (sysctlbyname("kern.osproductversion", b, &n, 0, 0) == 0) printf("kern.osproductversion = %s\n", b); }
	printf("== sizes\n");
	SZ(struct stat); SZ(struct statfs); SZ(struct dirent); SZ(struct termios); SZ(struct sigaction); SZ(siginfo_t); SZ(ucontext_t); SZ(mcontext_t); SZ(struct kevent); SZ(struct kevent64_s);
	SZ(struct flock); SZ(struct timeval); SZ(struct rlimit); SZ(struct rusage); SZ(struct sockaddr_in); SZ(struct sockaddr_in6); SZ(struct attrlist); SZ(struct msghdr); SZ(FTSENT); SZ(fsid_t); SZ(sigset_t);
	OFF(struct stat, st_mode); OFF(struct stat, st_ino); OFF(struct stat, st_atimespec); OFF(struct stat, st_birthtimespec); OFF(struct stat, st_size); OFF(struct stat, st_blocks); OFF(struct stat, st_flags);
	OFF(struct statfs, f_fstypename); OFF(struct statfs, f_mntonname); OFF(struct statfs, f_fsid); OFF(struct dirent, d_name); OFF(struct dirent, d_namlen); OFF(struct dirent, d_type); OFF(struct dirent, d_seekoff);
	OFF(FTSENT, fts_statp); OFF(FTSENT, fts_name); OFF(FTSENT, fts_path); OFF(FTSENT, fts_info); OFF(FTSENT, fts_parent); OFF(FTSENT, fts_number);
	OFF(struct kevent, filter); OFF(struct kevent, udata); OFF(struct sockaddr_in6, sin6_family); OFF(struct sockaddr_in6, sin6_addr); OFF(struct termios, c_cc); OFF(struct termios, c_ispeed);
	printf("== constants\n");
	CONST(AF_INET6); CONST(AF_UNIX); CONST(SO_NOSIGPIPE); CONST(MSG_NOSIGNAL); CONST(O_CLOEXEC); CONST(O_DIRECTORY); CONST(O_NOFOLLOW); CONST(O_EXLOCK); CONST(O_SYNC); CONST(O_DSYNC);
	CONST(AT_FDCWD); CONST(AT_SYMLINK_NOFOLLOW); CONST(AT_REMOVEDIR); CONST(AT_EACCESS);
	CONST(F_GETPATH); CONST(F_FULLFSYNC); CONST(F_NOCACHE); CONST(F_DUPFD_CLOEXEC); CONST(F_GETLK); CONST(F_SETLKW);
	CONST(MAP_ANON); CONST(MAP_FIXED); CONST(MAP_JIT); CONST(MAP_NORESERVE); CONST(MS_SYNC); CONST(MS_ASYNC); CONST(MS_INVALIDATE);
	CONST(SA_SIGINFO); CONST(SA_RESTART); CONST(SA_ONSTACK); CONST(SA_USERTRAMP); CONST(SIGUSR1); CONST(SIGBUS); CONST(SIGSYS);
	CONST(EVFILT_READ); CONST(EVFILT_USER); CONST(EVFILT_TIMER); CONST(EVFILT_MACHPORT); CONST(EVFILT_VM); CONST(EV_UDATA_SPECIFIC); CONST(NOTE_SECONDS); CONST(NOTE_USECONDS); CONST(NOTE_NSECONDS);
	CONST(ENOATTR); CONST(EOVERFLOW); CONST(ECANCELED); CONST(EILSEQ); CONST(EBADMSG); CONST(EPROTO); CONST(ENOTSUP); CONST(EDEVERR); CONST(EAUTH);
	CONST(TIOCGETA); CONST(TIOCSETA); CONST(TIOCGWINSZ); CONST(FIONREAD); CONST(RLIMIT_NOFILE); CONST(RLIMIT_AS);
	CONST(KERN_SUCCESS); CONST(MACH_RCV_TIMED_OUT); CONST(MACH_SEND_INVALID_DEST); CONST(MACH_MSG_TYPE_MAKE_SEND);
	printf("== sysctl\n");
	{ const char *names[] = { "hw.ncpu", "hw.activecpu", "hw.logicalcpu", "hw.physicalcpu", "hw.memsize", "hw.pagesize", "hw.cachelinesize", "hw.l1dcachesize", "hw.l2cachesize", "hw.tbfrequency", "hw.cpufrequency",
	    "hw.byteorder", "hw.cpu64bit_capable", "kern.ostype", "kern.osrelease", "kern.osversion", "kern.version", "kern.hostname", "kern.argmax", "kern.maxproc", "kern.maxfiles", "kern.maxfilesperproc", "kern.osrev", "kern.boottime", "hw.model", "hw.targettype", NULL };
	  int i; for (i = 0; names[i]; i++) { char b[512]; size_t n = sizeof(b); memset(b, 0, sizeof(b)); if (sysctlbyname(names[i], b, &n, 0, 0) != 0) { printf("%s: error %d\n", names[i], errno); continue; }
	    if (n == 4) printf("%s = %d (4 bytes)\n", names[i], *(int *)b); else if (n == 8) printf("%s = %lld (8 bytes)\n", names[i], *(long long *)b); else if (n == 16) printf("%s = %lld s %d us (16 bytes)\n", names[i], *(long long *)b, *(int *)(b + 8)); else printf("%s = \"%s\" (%zu bytes)\n", names[i], b, n); } }
	printf("== mach\n");
	{ mach_timebase_info_data_t tb; mach_timebase_info(&tb); printf("timebase %u/%u\n", tb.numer, tb.denom); }
	printf("task_self %#x host_self %#x thread_self %#x\n", mach_task_self(), mach_host_self(), mach_thread_self());
	{ mach_port_t p = MACH_PORT_NULL; kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p); printf("mach_port_allocate kr %d name %#x; reply port %#x\n", kr, p, mig_get_reply_port()); }
	printf("== calls\n");
	{ struct stat st; stat("/", &st); printf("stat(/): st_dev %#x st_mode %o st_nlink %u st_ino %llu st_blksize %d\n", (unsigned)st.st_dev, st.st_mode, (unsigned)st.st_nlink, (unsigned long long)st.st_ino, (int)st.st_blksize);
	  stat("/dev/null", &st); printf("stat(/dev/null): st_rdev %#x st_dev %#x\n", (unsigned)st.st_rdev, (unsigned)st.st_dev); }
	{ struct statfs sf; statfs("/", &sf); printf("statfs(/): f_bsize %u f_iosize %d f_fstypename %s f_mntonname %s f_flags %#x f_type %u\n", sf.f_bsize, sf.f_iosize, sf.f_fstypename, sf.f_mntonname, sf.f_flags, sf.f_type); }
	{ int fd = open("/", O_RDONLY); char buf[4096]; long long base = 0; int n = (int)syscall(344 /* getdirentries64 */, fd, buf, (unsigned long)sizeof(buf), &base); struct dirent *d = (struct dirent *)buf; printf("getdirentries(/): %d bytes, first entry reclen %u namlen %u type %u name %s\n", n, d->d_reclen, d->d_namlen, d->d_type, d->d_name); close(fd); }
	{ struct attrlist al; struct { u_int32_t len; u_int32_t objtype; } __attribute__((packed)) ab; memset(&al, 0, sizeof(al)); al.bitmapcount = ATTR_BIT_MAP_COUNT; al.commonattr = ATTR_CMN_OBJTYPE; int r = getattrlist("/", &al, &ab, sizeof(ab), 0); printf("getattrlist(/, OBJTYPE): r %d length %u objtype %u (VDIR=2)\n", r, ab.len, ab.objtype); }
	{ char p[1024]; int fd = open("/tmp", O_RDONLY); int r = fcntl(fd, F_GETPATH, p); printf("F_GETPATH(/tmp): r %d %s\n", r, r == 0 ? p : strerror(errno)); close(fd); }
	{ const char *f = "/tmp/probe_x.txt"; int fd = open(f, O_CREAT | O_WRONLY, 0644); int r = fsetxattr(fd, "com.test.x", "v", 1, 0, 0); char b[8]; ssize_t g = fgetxattr(fd, "com.test.x", b, sizeof(b), 0, 0); char l[64]; ssize_t ln = flistxattr(fd, l, sizeof(l), 0);
	  printf("xattr: set %d get %zd list %zd first name \"%s\"\n", r, g, ln, l); errno = 0; g = fgetxattr(fd, "no.such", b, sizeof(b), 0, 0); printf("xattr missing: %zd errno %d (%s)\n", g, errno, strerror(errno)); close(fd); unlink(f); }
	{ int fd[2]; pipe(fd); struct kevent ke, out; int kq = kqueue(); EV_SET(&ke, fd[0], EVFILT_READ, EV_ADD, 0, 0, (void *)0x1234); kevent(kq, &ke, 1, NULL, 0, NULL); write(fd[1], "z", 1); struct timespec ts = { 1, 0 }; int n = kevent(kq, NULL, 0, &out, 1, &ts); printf("kevent: n %d filter %d data %ld udata %p\n", n, out.filter, (long)out.data, out.udata); }
	{ struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_sigaction = on_usr1; sa.sa_flags = SA_SIGINFO; sigaction(SIGUSR1, &sa, NULL); kill(getpid(), SIGUSR1); }
	{ struct sigaction sa, old; memset(&sa, 0, sizeof(sa)); sa.sa_handler = SIG_IGN; sa.sa_flags = SA_RESTART; sigaction(SIGUSR2, &sa, &old); printf("sigaction: old handler %p\n", (void *)old.sa_handler); }
	{ struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl); printf("RLIMIT_NOFILE: cur %llu max %llu\n", (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max); }
	{ int s = socket(AF_INET6, SOCK_STREAM, 0); struct sockaddr_in6 a; memset(&a, 0, sizeof(a)); a.sin6_len = sizeof(a); a.sin6_family = AF_INET6; bind(s, (struct sockaddr *)&a, sizeof(a)); socklen_t l = sizeof(a); getsockname(s, (struct sockaddr *)&a, &l); printf("AF_INET6 socket %d: sin6_len %u family %u port %u\n", s, a.sin6_len, a.sin6_family, ntohs(a.sin6_port)); }
	{ int v = 1; int r = setsockopt(0, SOL_SOCKET, SO_NOSIGPIPE, &v, sizeof(v)); printf("setsockopt(0, SO_NOSIGPIPE): r %d errno %d\n", r, errno); }

	printf("== process structures\n");
	SZ(struct kinfo_proc); SZ(struct extern_proc); SZ(struct eproc); SZ(struct proc_bsdinfo); SZ(struct proc_taskinfo); SZ(struct proc_taskallinfo); SZ(struct proc_threadinfo); SZ(struct proc_vnodepathinfo); SZ(struct proc_fdinfo); SZ(struct vnode_fdinfowithpath);
	OFF(struct kinfo_proc, kp_proc); OFF(struct kinfo_proc, kp_eproc);
	OFF(struct extern_proc, p_starttime); OFF(struct extern_proc, p_flag); OFF(struct extern_proc, p_stat); OFF(struct extern_proc, p_pid); OFF(struct extern_proc, p_oppid); OFF(struct extern_proc, p_estcpu); OFF(struct extern_proc, p_pctcpu);
	OFF(struct extern_proc, p_priority); OFF(struct extern_proc, p_nice); OFF(struct extern_proc, p_comm); OFF(struct extern_proc, p_pgrp); OFF(struct extern_proc, p_xstat);
	OFF(struct eproc, e_paddr); OFF(struct eproc, e_sess); OFF(struct eproc, e_pcred); OFF(struct eproc, e_ucred); OFF(struct eproc, e_vm); OFF(struct eproc, e_ppid); OFF(struct eproc, e_pgid); OFF(struct eproc, e_jobc); OFF(struct eproc, e_tdev); OFF(struct eproc, e_tpgid); OFF(struct eproc, e_tsess); OFF(struct eproc, e_wmesg); OFF(struct eproc, e_xsize); OFF(struct eproc, e_xrssize); OFF(struct eproc, e_xccount); OFF(struct eproc, e_xswrss); OFF(struct eproc, e_flag); OFF(struct eproc, e_login); OFF(struct eproc, e_spare);
	OFF(struct proc_bsdinfo, pbi_status); OFF(struct proc_bsdinfo, pbi_pid); OFF(struct proc_bsdinfo, pbi_ppid); OFF(struct proc_bsdinfo, pbi_uid); OFF(struct proc_bsdinfo, pbi_comm); OFF(struct proc_bsdinfo, pbi_name); OFF(struct proc_bsdinfo, pbi_start_tvsec); OFF(struct proc_bsdinfo, pbi_nice);
	OFF(struct proc_taskinfo, pti_virtual_size); OFF(struct proc_taskinfo, pti_resident_size); OFF(struct proc_taskinfo, pti_threadnum); OFF(struct proc_taskinfo, pti_total_user);
	CONST(PROC_PIDTBSDINFO); CONST(PROC_PIDTASKINFO); CONST(PROC_PIDTASKALLINFO); CONST(PROC_PIDPATHINFO); CONST(PROC_PIDVNODEPATHINFO); CONST(PROC_PIDLISTFDS); CONST(PROC_PIDTHREADINFO); CONST(PROC_ALL_PIDS); CONST(PROC_PIDPATHINFO_MAXSIZE);
	CONST(KERN_PROC); CONST(KERN_PROC_ALL); CONST(KERN_PROC_PID); CONST(KERN_PROC_PGRP); CONST(KERN_PROC_UID); CONST(KERN_PROCARGS2); CONST(KERN_PROC_RUID); CONST(KERN_PROC_TTY); CONST(SIDL); CONST(SRUN); CONST(SSLEEP); CONST(SSTOP); CONST(SZOMB);
	{ int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() }; struct kinfo_proc kp; size_t n = sizeof(kp); memset(&kp, 0, sizeof(kp));
	  int r = sysctl(mib, 4, &kp, &n, NULL, 0); printf("sysctl KERN_PROC_PID: r %d size %zu pid %d ppid %d stat %d flag %#x comm %s uid %u ruid %u pgid %d tpgid %d login %s ngroups %d\n", r, n, kp.kp_proc.p_pid, kp.kp_eproc.e_ppid, kp.kp_proc.p_stat, kp.kp_proc.p_flag, kp.kp_proc.p_comm, kp.kp_eproc.e_ucred.cr_uid, kp.kp_eproc.e_pcred.p_ruid, kp.kp_eproc.e_pgid, kp.kp_eproc.e_tpgid, kp.kp_eproc.e_login, kp.kp_eproc.e_ucred.cr_ngroups); }
	{ int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL }; size_t n = 0; int r = sysctl(mib, 3, NULL, &n, NULL, 0); printf("sysctl KERN_PROC_ALL: r %d size %zu (%zu entries)\n", r, n, n / sizeof(struct kinfo_proc)); }
	{ char path[PROC_PIDPATHINFO_MAXSIZE]; int r = proc_pidpath(getpid(), path, sizeof(path)); printf("proc_pidpath: %d %s\n", r, path); char nm[64]; r = proc_name(getpid(), nm, sizeof(nm)); printf("proc_name: %d %s\n", r, nm); }
	{ struct proc_bsdinfo bi; int r = proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)); printf("proc_pidinfo TBSDINFO: r %d pid %u ppid %u status %u uid %u comm %s name %s nice %d start %llu\n", r, bi.pbi_pid, bi.pbi_ppid, bi.pbi_status, bi.pbi_uid, bi.pbi_comm, bi.pbi_name, bi.pbi_nice, (unsigned long long)bi.pbi_start_tvsec); }
	{ int pids[4096]; int r = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)); printf("proc_listpids: %d bytes (%d pids)\n", r, r / (int)sizeof(int)); }
	{ int mib[3] = { CTL_KERN, KERN_PROCARGS2, getpid() }; char buf[4096]; size_t n = sizeof(buf); int r = sysctl(mib, 3, buf, &n, NULL, 0); printf("KERN_PROCARGS2: r %d size %zu argc %d first string %s\n", r, n, r == 0 ? *(int *)buf : -1, r == 0 ? buf + 4 : ""); }
	printf("== done\n");
	return 0;
}
