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
  ~38 ms**. Its early-out guard byte `0x831D5335` is 0 at that point, so it runs and then
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

**Pivot to streaming (documented subsystem).** Using
`docs/GTA_IV_ASSET_STREAMING_SYSTEM.md`, probes show:

- subsystem init `sub_822D30B8` **runs** and returns 1 (caller `LR=0x821D04B0`);
- registry init `sub_822D3190` **never runs**, so `*(0x83032744)` (registry
  pointer) stays `0x00000000` for the whole run — nothing can stream;
- the registry chain is
  `sub_82141F00 -> sub_821FD340 -> sub_82570BC0 -> sub_821CFE98 -> sub_822D3190`,
  and **`sub_82141F00` is never called**;
- `sub_82141F00` is invoked only from the boot state dispatcher
  `sub_82142230` (five conditional call sites).

So the wall is now: the boot state machine never selects the branch that
initialises the streaming registry. Next step is the dispatch condition inside
`sub_82142230`.

**Root gate found.** `sub_82142230` (the real runtime/init that reaches
streaming) is called **only** from `sub_82140088`, after its wait loop exits:

```
sub_82140088:
  while (sub_821458B8() != 0) { Sleep(1); }   // <-- main thread is parked here
  sub_82142230();                             // real init, never reached
```

`sub_821458B8` returns `byte[0x831D5348]` when `byte[0x831D5335] != 0`. Both bytes
read **1** for the whole run, so the loop never exits and nothing downstream —
streaming registry included — ever initialises.

`0x831D5348` has only three writers, all in the 8214xxxx boot family:

- `sub_82142230` (never runs),
- `sub_82144188` (runs continuously from the render thread, `LR=0x821B5BF4`),
- `sub_82145420` (runs once, is a mode-setter).

In `sub_82144188` the clear is guarded by: a float comparison, the flag itself
being non-zero, **and a job/state field at `(r9+r30+8) == 3`**. That field never
reaches 3, so the flag is never cleared.

So the deadlock is: boot waits for the initial async load to report state 3 →
that load never runs because the task pool is never fed → and the pool is fed by
the runtime that only starts after the wait exits.

**Correction: there was no contradiction.** An earlier revision of this file
claimed `sub_82145420` skipped the loader-flag store. That was an
address-arithmetic error in the probe: `21304 = 0x5338`, so the store target is
`-3(r31)` = **`0x831D5335`**, not `0x831D5345`. Measured correctly, the function
sets `0x831D5335 = 1` (loader active) exactly as the code says, and it sets
`ready` (`0x831D5348`) to 1. Lesson recorded because it cost a whole chase: use
hex offsets in probes, never decimal offsets converted by hand.

**Actual state machine (measured):**

| byte | meaning | observed |
|---|---|---|
| `0x831D5335` | loader active, set by `sub_82145420` | **1** |
| `0x831D5348` | `ready`, polled by `sub_821458B8` | **1** (never clears) |
| `0x831D5340` | current load-record index | **0** |
| `0x831D54A0` | `record[0].state` (`base 0x831D5498`, stride 400, field +8) | **1**, never 3 |

The only writer that can clear `ready` while the game runs is `sub_82144188`,
guarded by `record[idx].state == 3`. That record stays at 1.

**Decisive measurement (whole I/O stack is up, but idle).**

| probe | result |
|---|---|
| `pgStreamer` init `sub_8284CFD8` | **runs** (1 call) |
| `pgStreamer::Open` `sub_8284CB80` | runs (8+ calls, all at init) |
| submit `sub_8284C888` | runs (8+ calls, all at init) |
| worker `sub_8284BF50` | runs (8+ calls, all at init) |
| title slot init `sub_82678EB0` | **runs once** (`LR=0x822D3168`) |
| **title load issue `sub_826793F0`** | **never called (0)** |

So the queued bulk-I/O layer works, the 8 title slots are initialised, and the
one function that would *issue* a load is never invoked. Nothing can therefore
advance a record to state 3, clear `ready`, or fetch the world.

Note the slot-init caller `0x822D3168` sits in the registry-init region — the
same chain (`sub_82141F00 → … → sub_822D3190`) that never runs. One missing
boot call explains every idle subsystem measured so far.

**Why the registry chain cannot run before the wait loop.** `sub_82142230` has
exactly one caller: `sub_82140088` line 141 — i.e. the statement *after* the
wait loop. Its first call site is gated on `state == 7`
(`cmpwi r29,7` at `0x821426F0`). So by construction the registry/streaming
initialisation runs only after `ready` clears.

That makes the initial load (the thing that must clear `ready`) a separate,
earlier mechanism which our port never starts. Everything below it is verified
working; the open question is now narrow and single:

> **Which code issues the *initial* load that clears `ready` before the wait
> loop exits, and why does it not run here?**

Answering that needs control-flow reading of the loader with a decompiler
(the `liberty-decomp`/Ghidra data is not in this checkout), not more probes.

**Experiment: clearing `ready` proves the block, but is not sufficient.**
Forcing `0x831D5348 = 0` once (labelled experiment, reverted afterwards):

- the boot wait loop exits and **`sub_82142230` is entered** for the first time
  (`[DISPATCH] sub_82142230 call#1 lr=0x821400C0 r3=0`) — so `ready` is
  confirmed as the gate;
- but the runtime then does nothing further: one call, no further log lines,
  `reg` stays 0, `presents`/`draws` stay 2/6, scene stays 0 even after 73 s.

So clearing `ready` unblocks the *sequencing*, yet the runtime still exits or
parks immediately. Its first streaming call site is gated on `state == 7`, and
the state it starts with (`r3 = 0`) never reaches 7 in this run.

Next question, narrowed again: **what advances `sub_82142230`'s state machine to
7** — and does it need the initial load, or something else entirely?

**Answer to "did it return": it never returns.** With
`--experiment-clear-ready` (off by default) the dispatcher is entered and then
blocks: `sub_82142230` logs entry and no exit. The main thread sits in
`sub_82A1A450` waiting on handle **`0xF8000044`**, which the object table
reports as a **Semaphore (type 8)**.

That semaphore is released exactly **once**, during init, from
`LR=0x82849890` (`[RelSem] ... handle=0xf8000044 delta=1`). By the time the
dispatcher reaches its wait — which only happens because the experiment let the
boot loop exit — the signal is already spent, so it blocks forever.

That is a release/consume ordering problem, and it matches the whole picture:
on hardware the dispatcher is already waiting when the loader signals.

**Who signals it (measured).** Hooking the release wrapper `sub_82849860` and
logging the caller's return address pairs both sides:

- releases to `0xF8000044` come from **`sub_821B5B20`** (`caller_lr` `0x821B5C48`
  and `0x821B5C1C`) — the render/submit loop that we measured running hundreds
  of times;
- the waiter is `LR=0x82849804` (the wait wrapper);
- only **two** releases happen, both during init.

So the semaphore is a "work ready" pulse between the submit loop and the
dispatcher. It is signalled at init and then never again, and the dispatcher
only starts waiting much later — after the boot loop finally exits.

This is a **boot-ordering fault, not a semaphore bug**: the consumer must be
waiting while the producer still pulses. Any fix that merely adds a signal would
be a gate; the real fix is to have the dispatcher running before/with the submit
loop.

**Debugger notes** (the binary ships an interactive guest debugger):
`vtable <object>` works, `threads` reports "No threads registered",
memory `watch` aborts the process (SIGABRT), and `break` stops the guest and
hangs the run. Next attempt should drive `break`/step with a proper follow-up
command sequence.

**Lavapipe cross-check (not driver-specific).** Running with
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` boots and presents
a frame, and every blocker reads exactly the same as on RADV:
`reg=0x00000000`, `loader_active=1`, `ready=1`, `jobs=0/0`, `evt` unsignalled,
`scene=0`. So the boot gate is guest logic, not Vulkan driver behaviour. Caveat:
under lavapipe the log pipeline stalls after the first frame, so RADV stays the
better backend for long observation; lavapipe is for ruling out the driver.

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
