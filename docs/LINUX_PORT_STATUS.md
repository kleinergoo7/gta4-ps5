# Linux port status

**Scope:** Linux is the target. Windows is future scope — keep fixes
platform-neutral (no PS5-only assumptions, no per-platform `#ifdef` logic in
shared files). The PS5 paths in this tree (`ps5/`,
`glue/rexglue-sdk-main/gta4-recomp/src/ps5/`, PS4/Switch branches) are not
maintained here: read-only debt, never "fix" them while working on Linux.

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

**Producer identified and blocked.** Thread `tid=20`, entry `0x821909D0`, never
returns from that function: it parks at `0x82190A28` inside
`KeWaitForSingleObject`. The waited object is `0x831D52F0`; its dispatcher
state is `0x01000000` (type byte 1, signal-state bit **0**) and it never
changes for the whole run. So the event is never signalled after the first
frames — the producer parks, which starves the render thread, which parks on
its own semaphore.

Semaphore releases do happen (`[RelSem]`, all at `LR=0x82849890`, delta 1) but
only during the init window and to other handles (`F800003C`, `F8000CC8`,
`F8000CD8`, `F8000878`, `F8000040`, …), never `F8001080` afterwards.

Next lead: find who signals event `0x831D52F0` — i.e. which guest code path was
expected to call `KeSetEvent` on it after the frontend finishes loading.

**Waker is never called.** The only external `KeSetEvent` on that event is at
`0x82190550` inside `sub_821904B8`; a 30 s run shows it is **never invoked at
all** (hook counter 0). Its only direct caller `sub_82190488` is also never
called directly. The address `0x82190488` is stored at two places in guest
memory: `0x820BE4C0` and `0x821089D0` (plus their `+0x10000000` mirrors), i.e.
it is only reachable as a **callback/vtable entry**, and no dispatcher reads it
in this run.

So the chain is: callback `0x82190488` → `sub_821904B8` → `KeSetEvent`
(`0x831D52F0`) → producer thread wakes → jobs flow → frames. The callback is
never invoked, which is why everything downstream sleeps.

Next lead: identify the table at `0x820BE4C0` (dump its slots and find which
virtual method index holds `0x82190488`) and the code that should dispatch it.

**Table characterised.** `0x820BE4C0` is a **vtable**, slot 0 = `0x82190488`
(neighbours: `0x8218FB70`, `0x8218FB90`, `0x8218FD60`, `0x829335B0`,
`0x82192460`, `0x821924B8`, `0x8218FF28`, `0x8218FE98`). Three live objects
carry that vptr (`0xB998C42C`, `0xD998C42C`, `0xF998B42C` — the same physical
object through guest RAM mirrors), so the object **is** constructed.

Constructors: `sub_821903B8` writes both `0x820BE4C0` and `0x820BE4DC` into the
new object, and `sub_821904B8` (the function that calls `KeSetEvent` on the
work event) also writes this vptr — so it is the object's **slot 0 method**
("notify / start"), not a standalone waker.

Conclusion: the object exists, the notify method is vtable slot 0, and no code
invokes it. The remaining question is purely: which dispatcher should call
`obj->vtable[0]`.

**Experiment: signal the work event manually.** `xeKeSetEvent(0x831D52F0)` from
the watchdog at t>=10 s returns 1 and *does* wake the producer: thread `tid=20`
moves from `LR=0x82190A28` to `LR=0x82190A84` and continues past the wait.
Signalling it every 500 ms keeps waking it, but `presents` stays at 2 and
`draws` at 6 — the producer wakes, finds an **empty work queue**, and parks
again. So the missing notify is not the root cause by itself: **no work is
being submitted**.

That moves the wall upstream: the guest thread that submits render work
(`828C5BA0 -> 821B3990 -> ... -> 828C15C8 -> 828C1228`) stops after two
frames. The next question is why that dispatch loop stops looping.

**Probes that followed (hypotheses eliminated):**
- `sub_821B5B20 -> sub_82144188` is called **continuously** (hundreds of times
  per run, always from `LR=0x821B5BF4`), so the submit loop is alive.
- The render gate `*(0x82B0B48C)` observes `-1` in steady state. Forcing it to
  `1` every 500 ms (`[GATEPOKE]`, experiment reverted) does **not** resume
  frames.
- `sub_828C15C8` is entered with a positive gate (3+ calls) and still emits no
  draws (`draws=6`, `[FRAME]` count 1).

So the dispatcher runs, the gate is open, and it renders nothing: the scene is
empty (`0x831C2458` stays 0). The wall is now **world/map streaming never
loads content**, which is consistent with the empty work queue.**

Next question: what submits streaming work — the task graph whose queue the
producer thread drains — and why it is never fed.

**Task pool located.** The global at `0x831D53EC` holds the pool object; it is
the same object as the `0x820BE4C0` vtable (mirror address `0xD998C42C`). Live
counters `+300`/`+304` read **0 / 0** for the whole run: nothing is ever
enqueued. Probe results:

- pool creator `sub_8218F4B8 -> sub_821901D8` runs **once** at boot
  (caller `LR=0x82A24A30`), so the pool and its worker exist.
- pool entry `sub_82193290` is **never** called.
- worker wake/registration `sub_821904B8` is **never** called.
- waking the worker manually (`xeKeSetEvent 0x831D52F0`) moves it past the wait
  but it finds an empty queue.

So the pool is created, the worker exists, and the game never submits or wakes
it. The remaining work is to find GTA IV's task-submit API (one of the 88
functions referencing `+21484`) and see why the streaming system never calls
it — a task-graph RE job, not a single probe.

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

## Ported upstream rexglue fixes

Vendored `glue/rexglue-sdk-main` predates upstream `v0.10.0` (Aug 2026).
Cherry-picked, platform-neutral, no oracle change so far:

- `ff43ff6b` — APCs delivered with guest context save/restore
  (`src/system/xthread.cpp`).
- `263a125b` — thread context value-initialized so `PPCContext` defaults apply
  (`include/rex/system/thread_state.h`, `src/system/thread_state.cpp`).
- `0bd1fd55` — POSIX access mode selected instead of ORing
  `O_RDONLY`/`O_WRONLY`; failed `pread`/`pwrite` report zero bytes; no forced
  `O_APPEND` (`src/core/filesystem_posix.cpp`).

## Upstream rexglue leads (not applied)

- `feat(ui)`: Wayland surface + `VK_KHR_wayland_surface`; `SDL_VULKAN` enabled
  on Linux; SDL video driver picked at runtime. Removes the
  `SDL_VIDEODRIVER=x11` requirement and the NULL-`Display` path.
- `fix(filesystem)`: POSIX access mode instead of ORing `O_RDONLY`/`O_WRONLY`
  (still broken here: `src/core/filesystem_posix.cpp:209-224`).
- `fix(runtime)`: value-initialize thread context (same class as the
  uninitialized-state boot crashes).
- `fix(system)`: APC delivery via trap frame, preserving guest registers.
- `fix(gpu)`: drop double-buffered page valid flags (candidate cause of frames
  stopping after two presents). Our `src/graphics/shared_memory.cpp` still has
  the pre-patch design; the change is ~140 lines, deferred until a wall needs
  it.
