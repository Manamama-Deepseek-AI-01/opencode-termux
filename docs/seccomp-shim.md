# libopencode-crhandler — seccomp shim behaviour

> Source: `tools/shim/sigsys_handler.c`. Built by `make seccomp-harden` and
> shipped automatically by `scripts/package/package_deb_native.sh` /
> `package_pacman_native.sh` as `lib/opencode/libopencode-crhandler.so`.
> Debug logging: `OPENCODE_CRSHANDLER_DEBUG=1` (silent by default).

## Why a shim exists at all

Android's app seccomp policy answers syscalls that are missing from its
allow-list with `SECCOMP_RET_TRAP` — the call is killed with `SIGSYS` instead
of returning `ENOSYS`. Bun uses several modern syscalls unconditionally on
Linux, so on older Android kernels the process dies instead of degrading.

The shim is linked as the **first `DT_NEEDED` entry** of the main ELF with
`RUNPATH $ORIGIN/../lib/opencode`, so it self-activates: no env var, no
launcher, no user action. Acceptance check:

```bash
readelf -dW "$PREFIX/bin/opencode" | grep crhandler     # must list the shim
```

## Three layers

| # | Layer | Covers | Answers with |
|---|---|---|---|
| 1 | PLT interposition (`syscall`, `close_range` exports) | call sites that go through libc | `-1`/`ENOSYS` without trapping |
| 2 | SIGSYS handler (`context` rewrite: `pc += 4`, `x0 = ret`) | inline `svc` sites, which cannot be interposed | per-syscall decision below |
| 3 | **Emulation** | traps where `ENOSYS` is fatal to the app | a working result |

Layer 1 exists because the forked child in bun's spawn path runs `close_range`
before `exec`, where signal handlers are not usable but an interposed
`syscall()` still is.

## Layer 3: emulated syscalls

| syscall | Kernel | Why `ENOSYS` is not enough | Emulation |
|---|---|---|---|
| `pidfd_open` 434 | Linux 5.3+ | bun's spawn treats a missing pidfd as a hard error (`Unknown Error, pidfd_open`) | pipe read-end returned as the fake pidfd; a watcher thread makes it readable when the process exits |
| `statx` 291 | Linux 4.11+ | project discovery never completes; surfaces as `ETIMEDOUT: connection timed out, stat '<path>'` | `newfstatat(79)` + a full `struct statx` fill |
| `close_range` 436 | Linux 5.9+ | nothing — graceful degrade is proven | plain `-ENOSYS` (layers 1 and 2) |
| `openat2` 437, `fchmodat2` 452, `clone3` 435, `faccessat2` 439 | newer | callers fall back on their own | plain `-ENOSYS` |

### pidfd emulation semantics

A pidfd is only ever *polled for readability*: it fires when the target exits.
So a pipe is a faithful stand-in if something closes its write end at exactly
that moment.

- The handler (async-signal context) creates the pipe and queues
  `{pid, write_end}` to the watcher over a non-blocking pipe — the only
  async-signal-safe channel available (no allocation, no locks).
- The watcher uses `waitid(P_PID, pid, WEXITED|WNOWAIT)`: it **observes the
  exit without reaping it**, so the exit status is still there for the
  application's own `waitpid()`. Nothing is stolen.
- Non-child pids (`pidfd_open` accepts any pid) fall back to a `/proc/<pid>`
  existence probe.
- Queue holds 64 entries, checks every 10 ms, one watcher thread per process.

Verified on-device: not readable before exit, readable at ~300 ms for a child
that sleeps 300 ms, and `waitpid()` still returns exit code 7 afterwards.

### statx emulation fidelity

Reported mask is `STATX_BASIC_STATS` (`0x7ff`), which is all `fstatat` can
supply. **Not** provided, by design: `stx_btime`, `stx_attributes` /
`stx_attributes_mask`, `stx_mnt_id`, and anything requiring `AT_STATX_*`
tuning hints. Verified field-by-field against the real `stat()` for a
directory, a regular file, a symlink, `/dev/null`, `/proc/self`, and a
nonexistent path (error case).

Flag words overlap for the bits that matter (`AT_SYMLINK_NOFOLLOW 0x100`,
`AT_EMPTY_PATH 0x1000`), so they are passed through rather than translated.

## Degradation rules

Everything is designed to fail toward "nothing happens", never toward a false
success:

- watcher unavailable or queue full → the fake pidfd simply never fires
  (the write end is deliberately left open; closing it would make the read end
  hit EOF and report a *fake* exit);
- `statx` emulation failure returns the real `fstatat` errno (`ENOENT`,
  `EACCES`, ...);
- unknown trapped syscall → `-ENOSYS`, as before.

## Verification recipe

```bash
# 1. build (what make seccomp-harden runs)
clang -shared -fPIC -O2 -o libopencode-crhandler.so tools/shim/sigsys_handler.c

# 2. exports must still be present
nm -D --defined-only libopencode-crhandler.so   # close_range, syscall

# 3. do the emulations match the real thing?
./sx_unit    # statx vs fstatat, 7 cases (dir/file/symlink/dev/procfs/missing)
./pd_unit    # pidfd: silent before exit, fires after, exit status preserved

# 4. per-syscall capability of a device
python3 one-syscall.py statx 291     # SIGSYS kill (rc 159) = trapped there

# 5. what traps during a real startup
OPENCODE_CRSHANDLER_DEBUG=1 opencode
```

## Field evidence (2026-10-24)

Onyx Boox NoteAir, Android 10, kernel `4.4.194`: `opencode` 2.0.31 (embedded
Bun 1.4.0) failed twice, in sequence —

1. `Error: Failed to start server` / `[cause]: Error: Unknown Error,
   pidfd_open` at `spawn` (fixed by layer 3 pidfd emulation);
2. `Error: ETIMEDOUT: connection timed out, stat '/.git'` at `Tui.run`, on
   nonexistent paths that the shell `stat`s in 24 ms (fixed by layer 3 statx
   emulation).

After both, the full TUI ran error-free for a full minute. The same binary is
unaffected on Android 11 / kernel 4.14, where `statx` exists and `pidfd_open`
is permitted — so the gap is the kernel, not the application. Older builds
that embed Bun 1.4.2+ (e.g. the `opencode2` legacy lineage) survive without
either emulation because that runtime falls back on its own.

## Relationship to the wrapper-line statx shim

`docs/README.md` also documents `libstatx-shim.so` with
`OPENCODE_DISABLE_STATX_SHIM=1` — that is the **wrapper appendix** lineage
(preloaded via launcher). The native mainline shim described here is not
disable-able at runtime by design: it is a `DT_NEEDED` dependency, and the two
never coexist in one install.
