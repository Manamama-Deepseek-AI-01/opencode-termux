// libopencode-crhandler: SIGSYS handler + PLT interposer + syscall emulation
// shim for Android seccomp traps.
//
// Why: bun uses syscalls missing from Android's zygote seccomp allowlist (and,
// on old kernels, missing from the kernel itself). Android answers disallowed
// syscalls with SECCOMP_RET_TRAP — the call is killed with SIGSYS instead of
// returning ENOSYS, so bun cannot degrade. Known traps on Android 10-14:
//   close_range 436   bun fd hygiene (startup + forked spawn child)
//   statx       291   libuv/bun fs.stat -> "ETIMEDOUT: ... stat '<path>'"
//   pidfd_open  434   bun spawn      -> "Unknown Error, pidfd_open"
//   openat2 437 / fchmodat2 452 / clone3 435 / faccessat2 439   (benign)
// Three layers handle this (docs/seccomp-shim.md is the design record):
//
// 1. PLT interposition: this library exports syscall()/close_range() and is
//    linked as the FIRST DT_NEEDED entry of the main ELF, putting it at the
//    head of bionic's symbol lookup order. close_range(436) is short-circuited
//    to -1/ENOSYS without trapping. This is the only layer that rescues the
//    spawn-child fd-hygiene sites (the forked child inherits no usable SIGSYS
//    disposition there). Every other syscall passes through to libc: on
//    kernels that implement it, the real call wins; on old kernels it traps
//    into layer 2.
//
// 2. SIGSYS handler: for inline-svc sites no interposition can reach. The
//    handler reads the syscall number from x8, emulates 434/291 in-process
//    (layer 3), and blanket-answers everything else with -ENOSYS (proven to
//    make bun fall back gracefully for openat2/fchmodat2/clone3/faccessat2).
//
// 3. Emulation (the -ENOSYS blanket is NOT enough for two calls — verified on
//    kernel 4.4 / Android 10, 2026-10-24, issue #37; re-verified 2026-11-03):
//      pidfd_open 434: bun's spawn treats a missing pidfd as a hard error
//        ("Unknown Error, pidfd_open") and the managed background server can
//        never start. Emulation: return the read end of a pipe; a watcher
//        thread closes the write end when the target exits, so the fake pidfd
//        becomes readable exactly at exit. Children are observed with
//        waitid(P_PID, ..., WEXITED|WNOWAIT|WNOHANG): WNOWAIT observes the
//        exit WITHOUT reaping it, so the application's own waitpid() still
//        receives the status. Non-children fall back to a /proc/<pid>
//        existence probe. Untracked/queue-full/unspawned-watcher fakes NEVER
//        fire (their write end stays open) — a false exit is worse than no
//        exit; nothing is ever reaped by this shim.
//      statx 291: project discovery never completes ("ETIMEDOUT ... stat
//        '<path>'"). Emulation: newfstatat(79) + a full kernel-ABI struct
//        statx fill reporting STATX_BASIC_STATS (0x7ff) — by design no
//        stx_btime/attributes/mnt_id. statx flag bits that newfstatat does
//        not accept (AT_STATX_* sync hints) are stripped; the overlapping
//        bits (AT_SYMLINK_NOFOLLOW 0x100, AT_NO_AUTOMOUNT 0x800,
//        AT_EMPTY_PATH 0x1000) pass through unchanged. Errors return the
//        kernel-style -errno (ENOENT surfaces as ENOENT, not a trap).
//
// Delivery: linked into the main ELF as the FIRST DT_NEEDED entry
// (libopencode-crhandler.so, self-activation; no env vars). The constructor
// runs during early loader init, before bun main() and before any JS executes.
//
// Bionic aarch64 ucontext (sys/ucontext.h -> asm/sigcontext.h):
//   struct sigcontext { __u64 fault_address; __u64 regs[31]; __u64 sp;
//                       __u64 pc; __u64 pstate; }
//   regs[0] == x0, regs[8] == x8 (syscall number at svc time). On
//   SECCOMP_RET_TRAP the context pc points AT the svc instruction (4 bytes on
//   aarch64); pc += 4 skips it, x0 fakes the syscall return. Bionic handler
//   return performs sigreturn automatically, applying the edits.
//
// Build (Termux NDK-style clang, aarch64 bionic):
//   clang -shared -fPIC -O2 -o libopencode-crhandler.so sigsys_handler.c
// Debug logging is opt-in: OPENCODE_CRSHANDLER_DEBUG=1 (default: silent).
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// SYS_SECCOMP is the only si_code seccomp TRAP can deliver for SIGSYS
// (kernel uapi siginfo.h); guard for header variance.
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

#ifndef ENOSYS
#define ENOSYS 38
#endif

// arm64 (generic) syscall numbers — independent of header vintage.
#define CR_NR_pipe2        59
#define CR_NR_newfstatat   79
#define CR_NR_waitid       95
#define CR_NR_statx       291
#define CR_NR_pidfd_open  434
#define CR_NR_close_range 436

// statx(2) request flags, kernel uapi statx.h.
#define CR_STATX_BASIC_STATS 0x000007ffU
#define CR_AT_SYMLINK_NOFOLLOW 0x0100
#define CR_AT_NO_AUTOMOUNT     0x0800
#define CR_AT_EMPTY_PATH       0x1000
#define CR_FSTATAT_FLAG_MASK (CR_AT_SYMLINK_NOFOLLOW | CR_AT_NO_AUTOMOUNT | CR_AT_EMPTY_PATH)
#define CR_AT_FDCWD (-100L)

// waitid(2) constants (kernel uapi; independent of <sys/wait.h> vintage).
#define CR_WNOWAIT 0x01000000
#define CR_WEXITED 4
#define CR_WNOHANG 1
#define CR_P_PID   1

// Cached once by the constructor (before any handler can run), so the
// handler itself never calls getenv/dlsym (async-signal-unsafe).
static int debug_cached = -1;
static int getenv_debug(void) {
    if (debug_cached < 0) debug_cached = getenv("OPENCODE_CRSHANDLER_DEBUG") ? 1 : 0;
    return debug_cached;
}

static int write_str(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return (int)write(2, s, n);
}

// Minimal decimal writer (no stdio): async-signal-safe.
static void write_num(const char *pre, long v, const char *post) {
    char buf[32];
    int i = (int)sizeof(buf) - 1;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    buf[i--] = '\0';
    if (u == 0) buf[i--] = '0';
    while (u) { buf[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (v < 0) buf[i--] = '-';
    write_str(pre);
    write_str(buf + i + 1);
    write_str(post);
}

// libc syscall(), resolved in the constructor; used by every emulation path
// (async-signal-safe: a plain syscall wrapper, no allocation, no locks).
typedef long (*real_syscall_t)(long, long, long, long, long, long, long);
static real_syscall_t real_syscall_cached;

static long cr_syscall(long nr, long a, long b, long c, long d) {
    long rc = real_syscall_cached(nr, a, b, c, d, 0, 0);
    if (rc == -1L) return -(long)errno;  // normalize to kernel-style -errno
    return rc;
}

// ---------------------------------------------------------------------------
// pidfd_open emulation state.
// ---------------------------------------------------------------------------

#define CR_QCAP 64
struct cr_qentry { long pid; int wfd; };
static int cr_queue_rfd = -1;  // watcher reads {pid, wfd} from here
static int cr_queue_wfd = -1;  // handler writes entries here (O_NONBLOCK)
static volatile int cr_watcher_ok = 0;

struct cr_live { long pid; int wfd; };
static struct cr_live cr_live[CR_QCAP];
static int cr_live_n = 0;      // watcher-thread-private

// Kernel-ABI struct statx writer: explicit offsets, no struct assumptions.
static void cr_stx_w32(unsigned char *b, int off, unsigned long v) {
    b[off]   = (unsigned char)(v);
    b[off+1] = (unsigned char)(v >> 8);
    b[off+2] = (unsigned char)(v >> 16);
    b[off+3] = (unsigned char)(v >> 24);
}
static void cr_stx_w64(unsigned char *b, int off, unsigned long v) {
    cr_stx_w32(b, off, v);
    cr_stx_w32(b, off + 4, v >> 32);
}

static unsigned long cr_dev_major(unsigned long dev) {
    return ((dev >> 8) & 0xfffUL) | ((dev >> 32) & ~0xfffUL);
}
static unsigned long cr_dev_minor(unsigned long dev) {
    return (dev & 0xffUL) | ((dev >> 12) & ~0xffUL);
}

// statx(291) emulation via newfstatat(79). Returns the kernel-style result:
// 0 on success, -errno on failure. Async-signal-safe (no allocation, one
// plain syscall, writes only the caller's buffer). dirfd/path/flags are the
// raw statx arguments; mask is intentionally ignored — every response reports
// STATX_BASIC_STATS, which is all fstatat can supply.
static long cr_emulate_statx(long dirfd, const char *path, long flags, void *buf) {
    if (real_syscall_cached == NULL) return -ENOSYS;
    if (buf == NULL) return -EFAULT;

    unsigned char st[160];  // bionic LP64 struct stat
    long nfa_flags = flags & CR_FSTATAT_FLAG_MASK;
    long rc = cr_syscall(CR_NR_newfstatat, dirfd, (long)path, (long)st, nfa_flags);
    if (rc != 0) {
        // Raw kernel result is already -errno; keep it kernel-style.
        return rc < 0 ? rc : -ENOSYS;
    }

    struct stat *s = (struct stat *)st;
    memset(buf, 0, 256);
    unsigned char *b = (unsigned char *)buf;
    cr_stx_w32(b, 0x00, CR_STATX_BASIC_STATS);                    // stx_mask
    cr_stx_w32(b, 0x04, (unsigned long)s->st_blksize);            // stx_blksize
    cr_stx_w64(b, 0x08, 0);                                       // stx_attributes (by design)
    cr_stx_w32(b, 0x10, (unsigned long)s->st_nlink);              // stx_nlink
    cr_stx_w32(b, 0x14, (unsigned long)s->st_uid);                // stx_uid
    cr_stx_w32(b, 0x18, (unsigned long)s->st_gid);                // stx_gid
    cr_stx_w32(b, 0x1c, (unsigned long)s->st_mode);               // stx_mode
    cr_stx_w64(b, 0x20, (unsigned long)s->st_ino);                // stx_ino
    cr_stx_w64(b, 0x28, (unsigned long)s->st_size);               // stx_size
    cr_stx_w64(b, 0x30, (unsigned long)s->st_blocks);             // stx_blocks
    cr_stx_w64(b, 0x38, 0);                                       // stx_attributes_mask
    cr_stx_w64(b, 0x40, (unsigned long)s->st_atim.tv_sec);        // stx_atime.tv_sec
    cr_stx_w32(b, 0x48, (unsigned long)s->st_atim.tv_nsec);       // stx_atime.tv_nsec
    cr_stx_w64(b, 0x50, 0);                                       // stx_btime (by design)
    cr_stx_w64(b, 0x60, (unsigned long)s->st_ctim.tv_sec);        // stx_ctime.tv_sec
    cr_stx_w32(b, 0x68, (unsigned long)s->st_ctim.tv_nsec);       // stx_ctime.tv_nsec
    cr_stx_w64(b, 0x70, (unsigned long)s->st_mtim.tv_sec);        // stx_mtime.tv_sec
    cr_stx_w32(b, 0x78, (unsigned long)s->st_mtim.tv_nsec);       // stx_mtime.tv_nsec
    cr_stx_w32(b, 0x80, cr_dev_major((unsigned long)s->st_rdev)); // stx_rdev_major
    cr_stx_w32(b, 0x84, cr_dev_minor((unsigned long)s->st_rdev)); // stx_rdev_minor
    cr_stx_w32(b, 0x88, cr_dev_major((unsigned long)s->st_dev));  // stx_dev_major
    cr_stx_w32(b, 0x8c, cr_dev_minor((unsigned long)s->st_dev));  // stx_dev_minor
    return 0;
}

// pidfd_open(434) emulation core: create the pipe pair and queue the write
// end for the watcher. Returns the read end (fake pidfd) or -errno.
// Async-signal-safe (two plain syscalls + a nonblocking pipe write).
static long cr_emulate_pidfd(long pid) {
    if (real_syscall_cached == NULL) return -ENOSYS;
    if (cr_queue_wfd < 0) return -ENOSYS;  // constructor failed: never fire

    int fds[2];
    long rc = cr_syscall(CR_NR_pipe2, (long)fds, O_CLOEXEC | O_NONBLOCK, 0, 0);
    if (rc != 0) return rc < 0 ? rc : -ENOSYS;

    struct cr_qentry e;
    e.pid = pid;
    e.wfd = fds[1];
    long wr = (long)write(cr_queue_wfd, &e, sizeof(e));
    if (wr != (long)sizeof(e)) {
        // Queue full/unavailable: the fake pidfd must NEVER fire (a false
        // exit is worse than no exit) and must never be reaped. Leave the
        // write end open; the read end blocks forever. Documented degradation.
        return fds[0];
    }
    return fds[0];
}

// Watcher thread: drains the queue and fires fakes at target exit.
static void *cr_watcher_fn(void *arg) {
    (void)arg;
    struct cr_qentry e;
    for (;;) {
        // Drain newly queued entries (queue pipe is O_NONBLOCK).
        for (;;) {
            long n = (long)read(cr_queue_rfd, &e, sizeof(e));
            if (n != (long)sizeof(e)) break;
            if (cr_live_n < CR_QCAP) {
                cr_live[cr_live_n].pid = e.pid;
                cr_live[cr_live_n].wfd = e.wfd;
                cr_live_n++;
            }
            // Live set full: leave the write end open — the fake never fires
            // (documented degradation; never a false exit).
        }
        // Sweep: WEXITED|WNOWAIT|WNOHANG observes exit without reaping.
        int i = 0;
        while (i < cr_live_n) {
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            long r = cr_syscall(CR_NR_waitid, CR_P_PID, cr_live[i].pid,
                                (long)&si, CR_WEXITED | CR_WNOWAIT | CR_WNOHANG);
            int gone = 0;
            if (r == 0) {
                gone = 1;  // exited; still waitable for the application
            } else if (r < 0) {
                long err = -r;  // cr_syscall returns raw -errno
                if (err == ECHILD) {
                    // Not our child: /proc/<pid> existence probe.
                    unsigned char stb[160];
                    char p[48];
                    char *w = p;
                    const char *pre = "/proc/";
                    const char *s2 = pre;
                    while (*s2) *w++ = *s2++;
                    unsigned long u = (unsigned long)cr_live[i].pid;
                    char digits[24];
                    int k = 0;
                    if (u == 0) digits[k++] = '0';
                    while (u) { digits[k++] = (char)('0' + (u % 10)); u /= 10; }
                    while (k) *w++ = digits[--k];
                    *w = '\0';
                    long pr = cr_syscall(CR_NR_newfstatat, CR_AT_FDCWD,
                                         (long)p, (long)stb, 0);
                    if (pr == -2 /* -ENOENT */) gone = 1;
                    // Any other error: conservatively treat as alive.
                }
                // EAGAIN (still running) and anything else: keep watching.
            }
            if (gone) {
                close(cr_live[i].wfd);  // read end becomes readable: fired
                cr_live[i] = cr_live[--cr_live_n];
                continue;
            }
            i++;
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000;  // 10 ms, per design
        nanosleep(&ts, NULL);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// SIGSYS handler (layer 2 + layer 3).
// ---------------------------------------------------------------------------

static void sigsys_handler(int signo, siginfo_t *info, void *uctx) {
    (void)signo;
    ucontext_t *uc = (ucontext_t *)uctx;
    if (info == NULL || info->si_code != SYS_SECCOMP || uc == NULL) {
        // Not a seccomp trap: restore default disposition (kill) and re-raise.
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGSYS, &sa, NULL);
        raise(SIGSYS);
        return;
    }
    unsigned long nr = uc->uc_mcontext.regs[8];  // x8 = syscall number
    int dbg = getenv_debug();

    if (nr == CR_NR_statx) {
        long rc = cr_emulate_statx((long)uc->uc_mcontext.regs[0],
                                   (const char *)uc->uc_mcontext.regs[1],
                                   (long)uc->uc_mcontext.regs[2],
                                   (void *)uc->uc_mcontext.regs[4]);
        uc->uc_mcontext.pc += 4;               // skip the svc
        uc->uc_mcontext.regs[0] = (unsigned long)(long)rc;  // kernel-style result
        if (dbg) write_num("CRHANDLER: SIGSYS nr=291 statx -> ", rc, "\n");
        return;
    }
    if (nr == CR_NR_pidfd_open) {
        long fd = cr_emulate_pidfd((long)uc->uc_mcontext.regs[0]);
        uc->uc_mcontext.pc += 4;
        uc->uc_mcontext.regs[0] = (unsigned long)(long)fd;
        if (dbg) write_num("CRHANDLER: SIGSYS nr=434 pidfd_open -> ", fd, "\n");
        return;
    }
    // Blanket answer: bun falls back gracefully on -ENOSYS for these.
    uc->uc_mcontext.pc += 4;                   // skip the svc
    uc->uc_mcontext.regs[0] = (unsigned long)(long)-ENOSYS;  // x0 = -ENOSYS
    if (dbg) write_num("CRHANDLER: SIGSYS nr=", (long)nr, " -> -ENOSYS\n");
}

__attribute__((constructor))
static void crhandler_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSYS, &sa, NULL) != 0 && getenv_debug()) {
        write_str("CRHANDLER: sigaction(SIGSYS) failed\n");
    } else if (getenv_debug()) {
        write_str("CRHANDLER: SIGSYS handler installed\n");
    }

    // Resolve libc syscall() up front: emulation paths (including the
    // async-signal-safe handler) must never dlsym on the trap path.
    real_syscall_cached = (real_syscall_t)dlsym(RTLD_NEXT, "syscall");
    if (real_syscall_cached == NULL) return;

    // Queue pipe + watcher thread. Constructor failure degrades gracefully:
    // pidfd fakes would never fire (never false-fire, per design).
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return;
    cr_queue_rfd = fds[0];
    cr_queue_wfd = fds[1];
    pthread_t th;
    if (pthread_create(&th, NULL, cr_watcher_fn, NULL) == 0) {
        pthread_detach(th);
        cr_watcher_ok = 1;
    } else if (getenv_debug()) {
        write_str("CRHANDLER: watcher thread unavailable — pidfd fakes will never fire\n");
    }
}

// ---------------------------------------------------------------------------
// PLT interposers. Only close_range (nr 436) is short-circuited here; statx
// and pidfd_open pass through so that kernels implementing them use the real
// calls, while trapping kernels fall into the emulating SIGSYS handler.
// ---------------------------------------------------------------------------

long syscall(long number, ...) {
    if (number == CR_NR_close_range) {
        if (getenv_debug()) write_str("CRHANDLER: syscall(436) -> -ENOSYS\n");
        errno = ENOSYS;
        return -1;
    }
    if (real_syscall_cached == NULL) {
        real_syscall_cached = (real_syscall_t)dlsym(RTLD_NEXT, "syscall");
    }
    if (real_syscall_cached == NULL) {
        // libc syscall() must exist (we are linked against it); fail loud in
        // debug builds rather than corrupting unrelated syscalls silently.
        write_str("CRHANDLER: dlsym(RTLD_NEXT, \"syscall\") failed\n");
        errno = ENOSYS;
        return -1;
    }
    va_list ap;
    va_start(ap, number);
    long a = va_arg(ap, long);
    long b = va_arg(ap, long);
    long c = va_arg(ap, long);
    long d = va_arg(ap, long);
    long e = va_arg(ap, long);
    long f = va_arg(ap, long);
    va_end(ap);
    return real_syscall_cached(number, a, b, c, d, e, f);
}

int close_range(unsigned int first, unsigned int last, unsigned int flags) {
    (void)first;
    (void)last;
    (void)flags;
    if (getenv_debug()) write_str("CRHANDLER: close_range() -> -ENOSYS\n");
    errno = ENOSYS;
    return -1;
}