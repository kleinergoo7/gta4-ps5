# Linux port status

State of the `linux-pc` branch (this repository's default branch) after the
`1.0.2` rebase. Read together with `AGENTS.md`: that file holds the standing
rules, this one holds where the port actually stands.

## What works

- **Clean build** on the reference machine (openSUSE Slowroll, RX 580, Mesa
  26.2.5 RADV, Brew Clang 23.1.3). See `AGENTS.md` for the exact configure
  flags; `LIBERTY_RECOMP_OPTIMIZE_TOOLS=OFF` and
  `CMAKE_CXX_SCAN_FOR_MODULES=OFF` are both required on Linux.
- **Install**, three ways:
  - headless CLI: `--install-iso <file|dir> --install-tu <file|dir>
    [--install-dlc-path <pkg>]`, exit codes 0/1/2;
  - native desktop setup (SDL message boxes + NFD pickers, always windowed);
  - the original ImGui wizard, still used by console builds.
- **Installed layout verified**: 6.6 GB under `~/.config/LibertyRecomp/game`,
  TU8 applied to `default.xexp`, `common.rpf` / `xbox360.rpf` / `audio.rpf`
  preserved next to their extracted trees (deleting them broke boot with
  `STATUS_NO_SUCH_FILE`).
- **Boot reaches rendering**: XEX loads, the VdSwap path runs, two frames
  present.

## Current wall

The game boots but never reaches the menu.

- The frame driver `sub_821412B8` is called **once** (from `8214004C`), so the
  main thread has no frame loop; it ends up in the post-init wait loop
  `sub_82140088` (`KeDelayExecutionThread`).
- `sub_82145420(1,0)` — the run entry called right after — **returns 0 after
  ~38 ms**. Its early-out guard byte `0x831D5345` is 0, so it runs and then
  falls through to the plain epilogue: it is a mode-setter, not the frame loop.
- The render thread (`821B5B20 -> 821C5BA0 -> 828C15C8`) parks on a semaphore
  (`0xF8001080`, `NtWaitForSingleObjectEx`) and is never woken.
  `presents=2`, `draws=6`, then nothing.
- After ~15 s every log source goes silent: no I/O, no streaming. This is a
  hard wait, not slow loading.

So no function in the boot chain that we have identified runs a frame loop.
The next question is where the loop is created: most likely a guest thread
that is started (or woken) by a path that a stub never reaches, or a job
semaphore whose producer never runs.

Open question: who signals that worker semaphore, and whether any of the
boot-unblocking gates below sit on the enqueue path that should signal it.

**Answered for the gates:** they are innocent. In a 40 s run every gate trigger
occurs inside the same ~400 ms window as the first frames
(27.2 s–27.6 s); after that, zero triggers. They are init-only and are not on
the steady-state path.

**Thread graph:** ~30 guest threads exist; most are parked in the generic
thread trampoline (`0x82849940` → `sub_82849928` → `sub_82A12C30`). Entry
points seen: `0x82849940` (trampoline), `0x82A11290` (module entry),
`0x821909D0`, `0x82A4F0E0`, plus `0x00000000` for host threads. No thread is
running a frame loop.

Remaining lead: find the owner/producer of semaphore `0xF8001080` (needs
object-table inspection late in boot, or hooking the semaphore release path).

## Boot-unblocking gates (all `WORKAROUND`, all logged)

Each one unblocks a deterministic host SIGSEGV during boot. All are in
`LibertyRecomp/gpu/video.cpp` unless noted.

| # | Site | Why |
|---|------|-----|
| 1 | `sub_82A49458` no-op | PM4 packet allocator; the stubbed ring never fills the `device+13232+16` slot, so the generated body dereferenced NULL+10941 |
| 2 | `sub_82A48B90` null gate | returns 0 for a NULL object; every caller null-checks the result |
| 3 | `sub_828D9768` no-op | NULL vtable slot with `0xDD`-pattern (freed/uninit) arguments; both callers ignore the return |
| 4 | `sub_82A488B8` zero-object gate | an all-zero input object produced a store to guest address 0 |
| 5 | `sub_827D5C80` no-op | `0xDD`-pattern inputs, NULL vtable slot, faulting computed store |

Plus `LibertyRecomp/kernel/arena_guard.cpp`: commit-on-demand for guard pages
hit through uninitialised stack slots (logs every commit and decline, 256 MB
budget, same-page guard).

Do not add a sixth silent gate. A new gate must log every trigger, carry the
`WORKAROUND` label, and name the condition that would let it be removed.

## Diagnostics for headless bring-up

- `[FRAME] change#N sig=0x... presents=... draws=...` — logged only when the
  frame's draw stream changes. A frozen boot produces exactly one line.
- `[LOADFLAGS] ... presents=... draws=...` — watchdog line every 5 s with
  loader flags, render gate and scene pointer.
- `[Wait1]`, `[FRAMEDRV]`, `[sub_82A12B60] caller LR=...` — one-shot probes used
  to identify the parked sites.

Success criterion for the current wall: `draws` climbs past 6 and new
`[FRAME] change` lines appear.

Note: literal pixel readback is not available. Swapchain images are created
without `TRANSFER_SRC` (`plume_vulkan.cpp:2341`) and copying the render target
faults inside plume, so the alarm is content-level (draw stream), not RGB.

## Reproduce

```
cmake --build build-linux --target LibertyRecomp
SDL_VIDEODRIVER=x11 ./build-linux/LibertyRecomp/LibertyRecomp --diagnostics \
  > /tmp/boot.log 2>&1
```

`SDL_VIDEODRIVER=x11` is required on a Wayland session: SDL opens a Wayland
window, the X11 display property is NULL, and plume's Xlib surface path passes
a NULL `Display` to the driver.

## Known issues not yet fixed

- Intermittent SIGSEGV during early init (input/XMA setup), roughly 1 in 4
  runs; timing-sensitive, does not reproduce under gdb.
- Each crash leaks a ~4.8 GB `xenia_memory_*` file in `/dev/shm`; when it fills,
  later runs die with SIGBUS in `Memory::Initialize`. Clean with
  `rm -f /dev/shm/xenia_memory_*`.
- Missing `LibertyRecompResources` drops the UI to the embedded default font
  (`UIFont` fallback in `ui/imgui_utils.cpp`).

## Not started

- Zig 0.16 modules (step 7 of the port plan).
- Upstreaming the Linux-safe parts (step 8).
- Vendoring an OFL tooling font to supersede the font fallback.
- Patching plume for `TRANSFER_SRC` to enable true pixel readback.
