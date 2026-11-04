# Bringing the opencode TUI up on an older Android / kernel

> Status: active ｜ Scope: v2 native line (`opencode`, 2.0.x–2.2.x) on bionic/Android
> 4.4–6.x kernels ｜ Last verified: 2026-11-04 on Android 10 (kernel 4.4.194) and
> Android 11 (kernel 4.14.186).
>
> Why this file exists: the same fix was rediscovered three times on the same
> device family, costing hours and two 77 MB transfers over a hotspot. The
> knowledge had been a *device state* (an installed `.so`) plus *prose* (chat,
> issue comments) — neither survives a `dpkg -i`. This page is the artifact that
> does. **Read it before touching a new device.**

## TL;DR for a third device

```sh
curl -fsSL https://raw.githubusercontent.com/Manamama-Deepseek-AI-01/opencode-tui-fix/main/opencode-tui-fix.sh | sh
opencode
```

The script fetches the newest official deb, sha256-verifies it, smoke-tests the
**stock** binary first and only then applies the minimum fix that device needs.
On a device that needs nothing it removes any stale shim and exits.
**Re-run it after every `dpkg -i` / `apt upgrade` of opencode** — the fixes live
in dpkg-owned paths and every package operation reverts them silently.

## Step 0 — record the baseline (30 s)

```sh
uname -a                                     # kernel line: 4.4.x vs 4.14.x matters
getprop ro.build.version.sdk                 # API level: 29, 30, 31...
dpkg-query -W -f='${Version}\n' opencode     # package revision, e.g. 2.2.5-2
```

## Step 1 — classify the failure

| Symptom | Family | Fix |
|---|---|---|
| `Error: Pointer exceeds safe integer range` at `createContext` / `Tui.run` | **Heap pointer tagging** (Android 11+, API 30+) | allocator-interposing shim + `LD_PRELOAD` wrapper (`noheap.c`) |
| `Error: Failed to start server` … `pidfd_open`, `Unknown Error`, or `EBADF: bad file descriptor` | **Seccomp/kernel trap gap** (kernel 4.4-class) | emulating crhandler v2 (`sigsys_handler.c`) |
| `ETIMEDOUT: connection timed out, stat '/.git'` | same trap gap, `statx` (291) | emulating crhandler v2 |
| parks at `Starting background server...`, **no UI, no error, stays alive** | same trap gap — *silent* variant | emulating crhandler v2 |
| exits instantly, nothing printed | not these families — check `opencode --version`, `dpkg -V opencode`, `readelf -dW $PREFIX/bin/opencode \| grep crhandler` | — |

## Step 2 — the rule that decides: *alive ≠ working*

This is the trap that produced two false "fixed" verdicts. A hung TUI looks
exactly like a working one if you only check that the process is still running.

```sh
TERM=xterm-256color timeout 75 script -qec "opencode --print-logs" /dev/null > "$PREFIX/tmp/s.log" 2>&1
wc -c < "$PREFIX/tmp/s.log"                       # rendered: ~10 KB+   parked: 80–230 B
tr -cd '\033' < "$PREFIX/tmp/s.log" | wc -c       # rendered: thousands  parked: 0
```

- **Pass** = screen-control sequences present (a rendered TUI emits them
  immediately), process alive at timeout.
- **Fail** = zero escape sequences, regardless of exit code or log text.
- Use a **75 s** window: on kernel 4.4 the failure can surface at ~36 s, which a
  25 s window misses entirely.

The installer implements exactly this rule, so trusting its verdict is safe;
"the process didn't die" is not a verdict.

## Step 3 — what the fixes are

1. **Stock package (preferred).** Prefer the newest revision: `2.2.5-1` fails on
   both Android 11 and Android 10; `2.2.5-2` (tag `Push261104`) fixes the
   Android 11 family (BigInt pointer conversion) and passes stock there.
   It does **not** fix the kernel-4.4 trap gap.
2. **Heap-tag shim** (`noheap.c`): strips the top-byte tag on the way out of the
   allocator and restores it before the real allocator sees the pointer
   (`mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, NONE)` is API 31+ only).
   Goes with a wrapper: `$PREFIX/bin/opencode` → `opencode.real` + `LD_PRELOAD`.
3. **Emulating crhandler v2** (`sigsys_handler.c`): `pidfd_open` (434) returns a
   real pipe read-end whose write end closes at child exit (`waitid WNOWAIT`),
   `statx` (291) is emulated with `newfstatat` + a kernel-ABI `struct statx`.
   The **shipped** `libopencode-crhandler.so` is the 8104-byte blanket
   `-ENOSYS` build, which this bun base cannot degrade from — that is the whole
   kernel-4.4 bug.

## Step 4 — verified matrix

| Device | OS / API | Kernel | Best package | TUI | Local fix needed |
|---|---|---|---|---|---|
| realme 8 (RMX3085) | Android 11 / 30 | 4.14.186+ | 2.2.5-2 | ✅ | none (stock) |
| Boox NoteAir (oscar) | Android 10 / 29 | 4.4.194-perf | 2.2.5-2 | ✅ | emulating crhandler v2 |

Upgrade path used on oscar: `2.0.31` → `2.2.5` (still broken) → `2.2.5-2`
(still broken, silently parked) → crhandler v2 → working.

## Step 5 — persists / does not persist

| Thing | Survives package upgrade? |
|---|---|
| Stock package + upstream fix | ✅ |
| `$PREFIX/lib/opencode/libopencode-crhandler.so` swap | ❌ reverted by every `dpkg -i` |
| `$PREFIX/bin/opencode` wrapper + `libnoheap.so` | ❌ `opencode upgrade` clobbers it; `dpkg` keeps it but it is stale after an upstream fix |

So: **do not run `opencode upgrade`** on these devices; upgrade through the
package manager and re-run `opencode-tui-fix.sh`. `--check` shows state without
changing anything, `--unwrap` returns to a plain stock binary.

## Where the pieces live

| Artifact | Location |
|---|---|
| Installer / diagnostic (self-retiring) | `opencode-tui-fix.sh` (this repo) · [public copy](https://github.com/Manamama-Deepseek-AI-01/opencode-tui-fix) |
| Heap-tag shim source | `tools/shim/noheap.c` (mirrored as `noheap.c` in the public repo) |
| Emulating crhandler source | `tools/shim/sigsys_handler.c` · `docs/seccomp-shim.md` |
| Upstream threads | Hope2333/opencode-termux issues #37 (crhandler design), #43 (Android 16), #46 (Android 12) |
| Session record / RCA | `install1.md` |

## Anti-patterns learned the hard way

- ❌ "Process alive at timeout" as a pass criterion — a parked TUI passes.
- ❌ 25 s smoke windows — the kernel-4.4 failure takes ~36 s.
- ❌ Assuming an upgrade applied a fix because the fix was "documented" — an
  untracked doc or a device-resident `.so` reaches nobody.
- ❌ `pkill -x opencode` from the host shell: a proot/guest install shares the
  real uid, so this kills the user's own session. Target PIDs explicitly.
- ❌ Re-downloading 77 MB "just in case": check `dpkg-query` first; the script
  skips the download when the installed version already matches the newest asset.
