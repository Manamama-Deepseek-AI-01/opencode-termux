# Upstream version bump — "how to upgrade it, for idiots"

> 状态：active ｜ 适用版本：v2 2.0.x–2.3.x / v1 1.18.x–1.19.x ｜ 最后核实：2026-11-13
>
> Scope: `opencode` (v2) and `opencode1` (v1) on Termux/aarch64. Last verified upgrading
> **2.2.5-2 → 2.3.0** on realme 8 (Android 11 / API 30, kernel 4.14.186+).
>
> Why this file exists: between 2.2.2 and 2.3.0 the *same three* failure classes were rediscovered
> on every bump (`0226f12`, `362f6ab`, `3c746db`), and the 2.3.0 attempt started by re-deriving
> the whole bionic renderer pipeline from scratch. Read this first; it is cheaper than the wheel.

---

## 0. TL;DR — pick a path

| Path | Line | Time | Needs | Result |
|---|---|---|---|---|
| **A. Wrapper** | appendix — **carries a glibc payload** | ~2 min | `npm`, `python3`, network | `$PREFIX/bin/opencode` = new version, runs today |
| **B. Native bionic** | **mainline — zero glibc, the project's premise** | hours | zig **0.16.0**, OpenTUI tree, ~2 GB RAM headroom | `opencode_<ver>_aarch64.deb`, dpkg-clean |

> [!IMPORTANT]
> **Path A is a stopgap, not the goal.** The wrapper embeds `ld-linux-aarch64.so.1`, `libc.so.6`,
> `libm.so.6` and `GLIBC_2.17` symbols (measured 2026-11-13: 2/8/4/7 occurrences) and execs that
> payload at runtime. Its load-time `NEEDED` is bionic, which makes it *look* glibc-free in
> `readelf -d` — do not trust that check alone. It is also ~234 MB against the native line's ~66 MB.
> Use it only to get onto a version, then replace it with path B.

**2.3.0 shipped via path A** (temporary, see §4). Path B is still un-built for 2.3.0.

First, three facts that stop wasted work:

1. **`opencode upgrade` never works here.** Measured 2026-11-13: `--method bun` spawns
   `bun install --global --trust --cache-dir <tmp> @opencode/cli@2.3.0`, bun answers
   `warn: skipping @opencode/cli@2.3.0: unsupported platform (wants os [ "linux", "win32", "darwin", ])`,
   installs **0** packages, exits **0**, and opencode prints `Upgrade complete` anyway.
   `~/.bun/bin` stays empty; `opencode --version` stays old. The npm package's `os` field excludes
   `android`, so on Termux it can never install.
2. **`pkg upgrade opencode` cannot help** unless a Hope2333 repo is configured *and* that batch
   actually ships the version. On 2026-11-13: no hope2333 apt/pacman source was configured
   (`apt-cache policy opencode` → candidate = installed, origin = dpkg status), and the newest
   published batch `Push261105` (2026-11-05) ships only 2.0.25–2.0.31 + `opencode1 1.19.5`.
   **No 2.3.0 package exists upstream of us.** Upstream latest = 2.3.0 (`npm dist-tags.latest`,
   tag `v2.3.0` in `anomalyco/opencode`).
3. **The installed `opencode` is a locally built dpkg package**, not an npm install: `opencode 2.2.5-2`
   aarch64, native bionic ELF (NDK r27c), 340 MB, `NEEDED libopencode-crhandler.so`.
   Nothing from npm will ever "update" it in place.

---

## 1. Path A — wrapper appendix (stopgap: has a glibc payload)

Takes upstream's **prebuilt glibc binary** and wraps it so bionic can exec it
(`bun-termux-loader`): the glibc `ld-linux-aarch64.so.1` + `libc.so.6` + `libm.so.6` travel
*inside* the 234 MB wrapper and are unpacked at runtime. **This is the appendix line, not the
project premise** — see the warning in §0.

```bash
cd ~/Downloads/GitHub/opencode-termux
./tools/produce-local.sh 2.3.0
```

Expected output (2026-11-13, 65 s wall):

```
[produce] downloading @opencode/cli-linux-arm64@2.3.0 from npm
[produce] cloning bun-termux-loader            # only if ~/bun-termux-loader is absent
[produce] wrapping for Termux
[+] Success! Output size: 234.3 MB
[produce] done: ELF 64-bit LSB pie executable, ARM aarch64, ... interpreter /system/bin/linker64, for Android 24 ...
[produce] version: opencode v2.3.0
```

Inputs/outputs:

| | |
|---|---|
| source package | `@opencode/cli-linux-arm64@<ver>` (v2 line; **v1 uses `opencode-linux-arm64`**) |
| tarball / unpacked | 119.5 MB / 245.7 MB |
| wrapped binary | `artifacts/opencode/runtime/opencode-termux`, 234.3 MB |
| cache | `~/.cache/opencode-termux/opencode-<ver>` (second run is instant) |
| disk needed | ~1 GB peak (tarball + extract + wrapped + cache) |

Install it (no deb exists — see §5 trap T2):

```bash
# rollback copy first — one cp is the whole rollback story
cp -p $PREFIX/bin/opencode ~/opencode-<oldver>-native-backup.bin
install -m755 artifacts/opencode/runtime/opencode-termux $PREFIX/bin/opencode
opencode --version          # must print opencode v<ver>
```

### Gate — "alive ≠ working" (mandatory)

```bash
TERM=xterm-256color timeout 75 script -qec "opencode --print-logs" /dev/null > /tmp/s.log 2>&1
wc -c < /tmp/s.log                                    # rendered: >=10 KB
tr -cd '\033' < /tmp/s.log | wc -c                    # rendered: hundreds-thousands
```

Measured for 2.3.0 on the realme 8: **211,500 B / 21,969 ESC** (fresh copy) and
**13,132 B / 709 ESC** (installed copy, idle after first paint), with
`\e[?1049h` alt-screen entry and `]0;OpenCode` title. A **parked** TUI is 80–230 B / 0 ESC.
Use the 75 s window: on kernel 4.4 the failure surfaces at ~36 s.

Confirm the version self-check inside the app too:

```bash
grep -ao 'current=[0-9.]* .*action=[a-z-]*' /tmp/s.log | head -1
# → current=2.3.0 ... action=up-to-date
```

### Known consequence (write it down, don't be surprised later)

`dpkg-query -W opencode` still reports the **old** version and `dpkg -V opencode` reports
`??5??????  /data/data/com.termux/files/usr/bin/opencode` (file modified). That is expected:
the wrapper replaced a dpkg-owned file. Rollback = `cp` the backup back.

---

## 2. Path B — native bionic (canonical, heavier)

Do this when you want a dpkg-clean, glibc-free `opencode`. Four things must be true,
all discovered the hard way:

### 2.1 Add the version to the pin table (two edits, not one)

`build/matrix/pins.sh` is the single source of truth. Read the pins **from upstream source**,
not from the published wrapper:

```bash
gh api "repos/anomalyco/opencode/contents/packages/cli/package.json?ref=v<VER>" --jq .content | base64 -d   # dependencies["@opencode/pty"]
gh api "repos/anomalyco/opencode/contents/package.json?ref=v<VER>" --jq .content | base64 -d               # workspaces.catalog["@opentui/core"]
```

Then add **both**:

```sh
# 1) matrix_pins_for  — pty opentui lib ffi effect
<VER>) printf '<pty> <opentui> opentui-<opentui> ot_ctx -\n' ;;

# 2) matrix_pty_pkg   — package scope, renamed upstream in 2.2.3
2.2.3|2.2.4|2.2.5|<VER>) printf '@opencode/pty' ;;
```

Skipping (2) is the classic silent-ish failure: an unlisted version falls through to
`@opencode-ai/pty` and the build dies with
`Cannot find module '@opencode-ai/pty-linux-arm64-gnu/bin/opencode-pty'`.

For reference, **2.3.0** moved both coupled deps:

| dep | 2.2.5 | 2.3.0 |
|---|---|---|
| `@opencode/pty` | 0.4.0 | **0.5.2** |
| `@opentui/core` (catalog) | 0.8.0 | **0.8.4** |

### 2.2 Build a bionic `libopentui.so` for the new opentui version

```bash
# needs: zig >= 0.17.0 (build.zig.zon .minimum_zig_version — the script's 0.16.0 default
#        is a stale w7b-era pin, override ZIG_BIN with the Termux zig), the OpenTUI native
#        tree (clone --branch v<opentui-ver>), ~2 GB free RAM
OPENTUI_ZIG_DIR=~/develop/OpenTUI-w7b-native/packages/native \
ZIG_BIN="$(command -v zig)" \
  bash tools/transplant/build-libopentui.sh
# → artifacts/transplant/opentui-bionic-<ver>/libopentui.so   (gitignored — commit nothing, but BACK IT UP)
```

The script applies all `patches/opentui/*.patch`, then **fails loudly** unless the built .so
satisfies the semantic guards (`@min(scissor.width, 0x7FFFFFFF)`, hit-grid clamps) and carries
`tlsdesc_shim_init`. Verify an existing artifact any time with:

```bash
bash tools/transplant/build-libopentui.sh --check <path>/libopentui.so
```

**Do not try to reuse an older renderer across a version that added FFI symbols.**
Measured 2026-11-13: 0.8.4's JS symbol table calls `ot_video_*`, `ot_audio_*`,
`ot_scene_set_video`; the 0.8.0 bionic .so (recovered from the installed 2.2.5 binary) exports
none of them, and the official 0.8.4 arm64 .so does. Reuse = runtime dlsym failure.

### 2.3 Source anchors

`make` must fail, never silently skip, when upstream moved an anchor: the
`app-assets.ts` skipBuild guard, the pty rename, and the FFI pointer patch
(`tools/build-bionic/apply-ffi-pointer.sh`, the Android 11+ tagged-pointer fix).
If one of these drifts, fix the anchor; do not set `ALLOW_UNGUARDED=1` for a ship build.

### 2.4 Build + package + install

```bash
make deb-native VER=<VER>          # refuses glibc renderer, refuses un-hardened binary
dpkg -i packing/dpkg/opencode_<VER>_aarch64.deb
bash opencode-tui-fix.sh           # device fixes live in dpkg-owned paths; re-run is mandatory
# then run the §1 render gate again
```

---

## 3. Drift detection (so a bump is never a surprise)

```bash
bash scripts/ci/probe-latest-opencode.sh --check    # exit 0 = in sync, 1 = drift, 2 = probe error
```

⚠️ Until 2026-11-13 this probe was **blind**: it read `@opencode-ai/pty` while upstream 2.2.3+
publishes `@opencode/pty`, so it died with `cannot read upstream pins at v2.3.0` and the pin table
silently froze at 2.2.5. Fixed in this commit. **If the probe errors, the table is not "in sync" —
it is unmeasured.**

---

## 4. Where 2.3.0 stands

| Line | Version | glibc? | State |
|---|---|---|---|
| installed `$PREFIX/bin/opencode` | **2.3.0** | **YES — wrapper appendix** (`ld-linux`, `libc.so.6`, `GLIBC_2.17` embedded) | ✅ TUI verified 2026-11-13, `up-to-date` self-check. **Temporary: replace with path B** |
| dpkg package `opencode` | 2.2.5-2 (native mainline, file modified) | no | rollback binary at `~/opencode-2.2.5-2-native-backup.bin` |
| native bionic build 2.3.0 | not built | no | needs §2.1 pins + §2.2 renderer (zig 0.16.0 + OpenTUI tree) |
| published package 2.3.0 | does not exist | — | newest batch `Push261105` tops out at 2.0.31 / 1.19.5 |

Verify which line you are on — **not** with `readelf -d` (bionic `NEEDED` on both), but by payload:

```bash
for m in ld-linux-aarch64.so.1 libc.so.6 GLIBC_2.17; do printf '%-24s %s\n' "$m" "$(strings -a $PREFIX/bin/opencode | grep -cx "$m")"; done
# native mainline → all zero (or incidental single hits from bundled JS/docs)
# wrapper appendix → 2 / 8 / 7
```

---

## 5. Device-state traps (the actual "wheel")

| # | Trap | Symptom | Fix |
|---|---|---|---|
| T1 | `opencode upgrade --method bun/npm/...` | prints `Upgrade complete`, nothing installed | never use it; see §0.1 |
| T2 | `make family-v2-wrapper` needs `~/.omo/evidence/opencode-v2-port/rc2-rebuild/old-wrapper-deb-control.txt` | `FATAL: control template missing` | the template is **device state**, not repo state — recreate it or install the wrapped binary directly (§1) |
| T3 | `build-libopentui.sh` defaults to zig **0.16.0** + `~/develop/OpenTUI-w7b-native` | confusing zig API/compile errors, or "zig 0.16 binary missing" | the 0.16.0 default is a **stale pin**: OpenTUI v0.8.4's `build.zig.zon` declares `.minimum_zig_version = "0.17.0"`. Clone `--branch v<ver>` and pass `ZIG_BIN=$(command -v zig)` (Termux 0.17.x is correct) |
| T4 | `artifacts/` is gitignored | bionic `libopentui.so` and built runtimes vanish on cleanup, then get rebuilt from scratch | back the .so up somewhere durable before cleaning |
| T5 | `opencode-tui-fix.sh` fixes live in dpkg-owned paths | fixes silently reverted by every `dpkg -i` | re-run it after every package operation (verdict = render gate) |
| T6 | wrapper replaces a dpkg-owned file | `dpkg -V opencode` shows modified file | expected; rollback = `cp` the saved native binary back |
| T7 | two npm names for the platform binary | `ETARGET` on v2 | v1 = `opencode-linux-arm64`, v2 = `@opencode/cli-linux-arm64` |

---

## 6. Repo & fork layout (do not rediscover)

| Remote | URL | Role |
|---|---|---|
| `origin` | `Hope2333/opencode-termux` | upstream, **read-only for us** (no push access) |
| `fork` | `Manamama-Deepseek-AI-01/opencode-termux` | ours, writable |

Branch `native-android` is configured to **pull from `origin`, push to `fork`**. So:

```bash
git pull            # upstream updates
git push            # lands on our fork
```

Our local-only assets (not upstream, do not lose them): `tools/shim/sigsys_handler.c`
(crhandler v2 layer-3 emulation), `tools/shim/noheap.c`, `opencode-tui-fix.sh`, `install1.md`,
`docs/seccomp-shim.md`, `docs/reference/android-tui-bringup.md`, and this file.
