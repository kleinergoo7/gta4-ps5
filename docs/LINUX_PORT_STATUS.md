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

**Option (a) is not available — measured.** The manager object
(`this=0xDB56D2B0`) has an intact vtable at `0x8200DD84`, and its per-frame
update entries are slots 5, 6 and 8 (`0x82679DB8`, `0x82679F48`, `0x82679E50`).
Probes on all three show **zero calls** in a whole run. So there is no guest
call site to repair: nothing in this boot ever dispatches the manager's update.

Consequence: the drive point must come from the port (option (b)), and it must
be labelled as a workaround whose root cause is "runtime dispatcher unreachable
because boot waits on a flag the loader can never clear".

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

All runs go through weston (project policy):

```
cmake --build build-linux --target LibertyRecomp
tools/run_weston.sh --diagnostics > /tmp/boot.log 2>&1
```

`tools/run_weston.sh` keeps one long-lived weston nested in the outer X11
display (`DISPLAY=:1` by default), runs it with `--xwayland`, reads the inner
display number from the weston log and runs the game with
`SDL_VIDEODRIVER=x11` on that display. Only the game is killed; weston stays
up. The headless weston backend is wrong: it exposes no DRI3 and Mesa then
refuses to present (`No DRI3 support detected - required for presentation`).
`SDL_VIDEODRIVER=x11` is still required: SDL on a Wayland session opens a
Wayland window, the X11 display property is NULL, and plume's Xlib surface path
passes a NULL `Display` to the driver.

## Manager pump experiment: disproved (measured)

The streaming manager object is `this=0xDB56D2B0` with vtable `0x8200DD84`.
Its per-frame entries are slots 5/6/8 (`0x82679DB8`, `0x82679F48`,
`0x82679E50`), and none is ever dispatched. Driving them by hand from the
`sub_82144188` hook (off-by-default flag, since removed) gives:

| slot | result |
|---|---|
| 8 `82679E50` | SIGSEGV inside `sub_825132D0` (job-pool alloc) |
| 5 `82679DB8` | SIGSEGV, same path (`82679DB8 -> 82679B98 -> 82513F10 -> 825132D0`) |
| 6 `82679F48` | safe but inert: 3 pumps, then no further calls; state/ready unchanged |

Cause: the streaming pool global `0x82A9AA7C` is **zero for the whole run**
(`p0`, `p16`, `p56` all 0 measured every 5 s). The pool is never constructed,
so the manager cannot be pumped at all. The subsystem chain that would build it
(`sub_82141F00 -> sub_821FD340 -> sub_82570BC0 -> ...`) is called **only** from
`sub_82142230`, which never runs. Pumping is therefore not a viable drive point.

## Record state: armed once, never advanced (measured)

A hardware watchpoint on `record[0].state` (`0x1831D54A0`, guest base
`0x100000000`) over a whole run records **exactly one write**:

```
state 0 -> 1, backtrace:
  sub_82145968 <- sub_82145420 <- sub_82140000
```

`sub_82145420` is the boot mode setter: it arms the initial load by setting
`loader_active`, `ready` and `record[0].state = 1`, then the main thread parks
in `sub_82140088`. Nothing in the rest of the run ever writes the record again,
so no guest worker picks the record up. `sub_82144188` clears `ready` only at
`state == 3`, which therefore never happens.

`
## Boot-stage dispatcher: two iterations, then nothing (measured)

`sub_82142230` is not a flat init: it is a stage machine that loops on a stage
number and dispatches through a jump table. Stage 0 reads `sub_822414E8`
(returns **1**), which sets the stage to 1 and loops. Probes on the stage
entries show exactly **two** iterations of the loop body (`sub_821428C8`
completes both times, all of its callees return), and then no further entries:
`sub_8223DDA8` (stage 1) and `sub_8223CAD8` (stage 3) are never called. The
stage-7 branch at `loc_82142758`, the only place that calls `sub_82141F00` and
therefore starts the streaming registry, is never reached.

Consequence: clearing `ready` is not sufficient. Even with the wait loop
short-circuited, the boot sequencer stops after two iterations instead of
advancing to the stage that builds the registry.

## The boot stall is a frame-complete wait (measured)

Entry/exit pairing on the stage-machine loop body found the exact blocking call:

```
sub_82142230 (stage machine)
  -> sub_821428C8            (loop body, iteration 2 never returns)
       -> sub_821B5890       (per-frame scene update, #2 never returns)
            -> sub_828497D8  (NtWaitForSingleObject, infinite)
                 handle = [scene + 4024] = 0xF8000044
```

`sub_828497D8` is the wait wrapper whose LR `0x82849804` matched the earlier
`[Wait1]` probe, so both observations are the same object.

`0xF8000044` is released **only** from `sub_821B5B20` (the submit/render call,
caller LRs `0x821B5C48` and `0x821B5C1C`), and that function is called from
`sub_821B6F18`. In a whole run `sub_821B5B20` runs **once**, at startup; there is
no second release, so the dispatcher parks forever on the frame-complete wait,
the boot stage machine never advances past stage 2, `ready` never clears and the
streaming registry is never built.

`sub_821B6F18` has **no direct caller** in the recompiled corpus. A gdb
breakpoint settles what it is: its frame is

```
sub_821B6F18 <- sub_82849940 <- sub_82A21600 <- XThread::Execute
```

`sub_82849940` is the guest thread trampoline, so `sub_821B6F18` is a **thread
entry point**, not a frame callback and not a table-dispatched command. That
thread performs one submit and exits, which is why the frame-complete semaphore
is pulsed exactly twice and never again.

The 0x82109320 region that also contains `sub_821B6F18` is therefore not its
dispatcher; the pairs there are unrelated to this wall.

## Guest thread inventory (measured)

`sub_82849940` copies a 44-byte start block, then calls `[block+0]` with
`[block+4]`. Hooking the thread start (`sub_82A21600`) and reading that block at
start time recovers the real entry for every guest thread, which the periodic
watchdog cannot see (it only ever observes the trampoline `0x82849940`).

22 guest threads start in a run. Entries:

| entry | threads | note |
|---|---:|---|
| `0x8285D368` | 5 | worker pool (distinct args) |
| `0x82798BA0` | 3 | worker, shared arg `0x82FEA470` |
| `0x829FF398` | 2 | worker |
| `0x8284C5E8` | 2 | `pgStreamer` I/O workers (arg 0/1) |
| `0x4006E5C0` | 2 | XAM/pool |
| `0x821B6F18` | 1 | **the submit thread** — calls `sub_821B5B20` once |
| `0x82902728` `0x825FBB68` `0x82556188` `0x823A05E8` `0x82299490` `0x82212F38` `0x821BA088` | 1 each | single-purpose threads |

`0x821B6F18` is a thread entry, so the frame-complete pulse depends on a thread
that submits once and exits. No other guest thread releases `0xF8000044`.

## The frame signal is not lost — it is not produced (measured)

`sub_828497D8` is `NtWaitForSingleObject(handle, -1, null)`; its result is
`1` when the wait is satisfied. Hooking it and pairing with the release log
gives, for handle `0xF8000044`:

- **4 waits satisfied** (`ret=1`), then the 5th blocks (a blocked wait never
  returns, so it never logs);
- **2 releases**, both from `sub_821B5B20` at startup.

So the semaphore is created with an initial count that covers the first waits,
and after `initial + 2` waits it runs dry. Nothing is swallowed and the object
identity is right; the per-frame release simply never happens.

The port *does* raise vblanks: `GraphicsSystem` runs a `GPU VSync` worker that
calls `MarkVblank()` at the guest refresh rate. Those vblanks do not drive the
guest path that releases `0xF8000044`.

## Correction: the frame wait is downstream of `ready` (measured)

Signalling `0xF8000044` from the present path (two releases, one per presented
frame) changed **nothing**: `ready` stayed 1, `reg` stayed 0, the dispatcher
`sub_82142230` was never entered (`sub_821428C8` never called), and only two
presents ever happen, so at most two signals are produced.

Why: the `sub_82142230 -> sub_821428C8 -> sub_821B5890 -> wait(0xF8000044)`
chain measured earlier is only reachable **after** `ready` clears, which is a
`--experiment-clear-ready` run. In a normal run the outer wait loop in
`sub_82140088` still spins on `ready`, so the frame semaphore is not the current
wall at all — it is the first wall after `ready`.

The workaround was removed (policy: a workaround that does not move the wall is
reverted, not extended). Current wall remains: `record[0].state` never leaves 1,
so `ready` never clears.

## Record writer bails on two boot-zeroed gates (measured)

`sub_82145968` (the only writer of the title load record) returns 0 without
writing. Entry/exit probe with guest reads gives:

```
in:  r3=0x1 (leftover), g534B=0, g5349=1, tbl=[0x831E4DD4]=0xD9137880, cnt=*(tbl+328)=0
out: ret=0, rec+0=0x1770, rec+8=1, rest 0
```

Two gates, both zeroed by the boot path:

1. `g534B (0x831D534B) == 0` — stored by `sub_82145420` from its `r4` arg.
   Boot calls `sub_82145420(1,0)`, so the flag is 0 and the writer takes its
   first early exit. Forcing it to 1 (reverted experiment) still returns 0.
2. `cnt == 0` — `sub_821411D8` allocates the table (`tbl` non-null) but stores
   `r30=0` as the entry count at `+328`. Nothing later fills it, so the writer
   takes its second early exit.

So the record is bare because the boot initialises its table with zero entries,
not because the writer is broken. The filler of that table (`sub_82140748`,
called from the same init) is the next unread code on this path.

Note: an earlier probe read the table from the wrong base (`0x831D4DD4`,
off by `0x10000`). Correct base is `0x831E4DD4` (`lis -31970 + 19924`).

## Second load table is empty by construction (measured)

Watchpoint on the global `tbl = [0x831E4DD4]` shows exactly one write per run,
from `sub_821411D8` (via `sub_82140000` at boot):

```
tbl = second_tbl (376 B block, e.g. 0xD9137880)
*(second_tbl + 328) = count = 0
```

`sub_821411D8` allocates two blocks: 1656 B (`first_tbl`, e.g. `0xD91370B0`,
stored to global `0x831E4DD0`) and 376 B (`second_tbl`, stored to global
`0x831E4DD4`). It stores `r30 = 0` as the count at `second_tbl + 328`.
The filler it calls, `sub_82140748`, receives `first_tbl`, not `second_tbl`,
so it cannot fill the count the record writer reads.

A corpus search finds exactly one `sth ...328(...)` in all generated code:
the zeroing store in `sub_821411D8` itself. No CPU code ever increments the
second-table count. Either entries are added via computed addresses the
literal search misses, or the appender lives downstream of the boot wait.

The record writer therefore reads an empty table at boot by construction.
This is the bottom of the record-writer branch: the wall is not the writer,
the flag, or the table pointer — it is the missing table entries, whose
producer has no reachable call site while `ready` stays 1.

## Table appender unreachable while ready stays 1 (measured)

`sub_82219918` runs once at boot, right after the second-table store. Probe:

```
sub_82219918 #1 tbl=0xD9137880 cnt=0
```

Count unchanged. Combined with the previous round (filler gets the first
table, no literal `+328` store anywhere else), the producer of the second
table entries has no reachable call site while `ready` stays 1. The
record-writer branch is fully mapped and dry: writer healthy, flag zero,
table allocated, entries never appended.

Next action is not another probe. It is a decompiler view of
`sub_82219918` / `sub_82140748` to find the appender by computed address,
or a comparison against a port that already boots (PS5 branch).

## Decompiler view obtained (Ghidra 12.2, raw PPC BE32 base 0x82000000)

Guest image dumped once (`0x82000000` + 18 MB, holes zeroed) and imported as
`PowerPC:BE:32:default`. Anchor verified: file offset `0x145968` disassembles to
`mflr r12; bl 0x829FF790; stfd f31,-160(r1)` — the `sub_82145968` prologue from
the generated C++. Headless decompile (`MkFunc` + `Dec` scripts) gives 4 of 5:

- `sub_82219918`: zeroes a TLS struct, calls `sub_825FBE78`, then submits an
  async request via `sub_8284D220`. Never touches the load-table count.
  Eliminated as the appender.
- `sub_82140748`: copies asset-type names from `0x820B8E7C` into the first
  table: `DELAYED_IDE`, `DELAYED_IPL`, `ANIMGRP`, `HANDLING`, `VEHICLEEXTRAS`,
  `GXT`, `PLAYER`, `CARCOLS`, `PEDGRP`, `CARGRP`, `RADIO`, `RADIOLOGOS`,
  `RADARBLIPS`, `WEAPONINFO`, `THROWNWEAPONINFO`, … First table = type-name
  registry. Never touches the second table count.
- `sub_821411D8`: two allocations confirmed (1656 B first, 376 B second),
  count zeroed at `+328`. Matches the watchpoint result.
- `sub_82145420`: writes `ready`, `loader_active`, record base — matches the
  probe log. Its tail spins on a table (`do { } while`) the raw import cannot
  follow (bad-instruction truncation).
- `sub_82145968`: decompile FAILED (needs full auto-analysis, not the
  disassemble-window shortcut). Its logic is already read from the generated
  C++ instead: iterate second-table entries, build a name, `stricmp` against
  literals, write the record.

The dump code was removed after the image was obtained
(`/tmp/opencode/guest_82.bin` kept outside the repo).

## Init completes with an empty second table (measured)

All five calls after the second-table store run in one boot and leave the
count at zero:

```
sub_82219918 #1 tbl=0xD9137880 cnt=0
sub_82288550 #1 tbl=0xD9137880 cnt=0
sub_821F52C8 #1 tbl=0xD9137880 cnt=0
sub_822233A8 #1 tbl=0xD9137880 cnt=0
sub_82220118 #1 tbl=0xD9137880 cnt=0
```

So `sub_821411D8` (alloc + zero + store + filler + post-init) completes
without appending a single entry. The Ghidra view agrees: its decompiled body
allocates both tables, zeroes the count, calls the filler (first table only)
and the four tail calls, none of which touches `+328`.

The circle is now closed from both sides: the record writer needs table
entries that only code downstream of the boot wait can append, and the boot
wait needs the record the writer cannot fill. Breaking it needs one drive
point supplied by the port (table entries, manager pump, or frame pulse),
not another probe.

## Breakthrough: stage machine reaches stage 7, registry + audio up (measured)

With `--experiment-clear-ready --experiment-frame-signal` combined (the signal
must be a ~60 Hz vblank-emulation ticker, not per-present — presents stall at
2, so a present-driven signal runs dry after 2 releases):

- stage loop spins (`sub_821428C8` exits, `sub_821B5890` returns), reaching
  stage 1 (`sub_8223DDA8` x200), stage 2 (`sub_8223DEE8`), stage 4
  (`sub_822440F8` x2);
- stage-7 branch fires: `sub_82141F00 enter lr=0x82142710` — first time ever;
- registry chain runs: `sub_821FD340` and `sub_82570BC0` enter and exit,
  `sub_822D3190` exits with ret = manager object, `reg = 0xDD660350`;
- `sub_825120E8` inserts requests (#1–7, first ever);
- audio initializes (XMA decoder thread, `Audio system initialized`) and the
  title theme plays.

Still frozen: `presents=2 draws=6`, `scene=0`, `loadstate=1`.
`sub_82141F00` entered once and had not exited at t=42 s — it is doing the
long registry/streaming build or is parked on the next wait. That is the
current wall.

Neither flag is sufficient alone: clear-ready without the ticker parks stage 2
on `0xF8000044`; the ticker without clear-ready never reaches the dispatcher.

## Reframe: the game runs, the renderer and loader do not (measured)

With both flags, the picture changed from "boot stuck" to "game running":

- stage machine spins (frame waits fed by the ticker), reaches stage 7;
- `sub_82141F00` runs its registry chain (`reg = 0xDD660350`), then its long
  tail (`sub_821417C8`, 89 calls) executes dozens of init functions;
- audio initializes, theme plays; `sub_825120E8` inserts requests;
- resource cache paths execute (refill via `sub_82146600`, release via
  `sub_82287700`, list walk via `sub_821CECC0`).

Frozen: `presents=2 draws=6`, `scene=0`, `loadstate=1`, pool `0x82A9AA7C=0`.
The render thread (`sub_82144188`) spins but its submit branch
(`record.state==3`) never fires, and the title load (`sub_826793F0`) never
issues. The load path (manager pump + streaming pool + record machine) is
dead while the game logic around it runs.

Two failure shapes seen, timing-dependent: a hang in the release path
(`sub_82287700 #2` via the refill, no exit, no wait) and a crash on a garbage
object through an indirect call (`821CFC48 -> ... -> 821464B8 -> bctrl ->
82287700`, first word `0xCADA8000`). Both smell of init running on
half-built state rather than a single broken function.

Next: who builds the streaming pool. `sub_82570BC0` ran without building it;
the remaining static references (`sub_82A703D0`, `sub_82586EE8`,
`sub_821D0558`) get probes.

## Release path hangs on stale indices, crashes on garbage (measured)

`sub_82287700` (refcount release + indirect virtual cleanup) is reached by two
routes, and both are sick:

- via `821F6098 -> 82146600`: `82287700#1 obj=0x14A0` returns, `#2 obj=0x4`
  never returns. No kernel wait follows (main thread stops waiting entirely),
  so it spins or deadlocks in user code. Callers pass small integers as the
  object: the chain is `821417C8 -> 821D4900 (ret) -> 821F6098 (r3) ->
  82146600 -> 82287700`, i.e. 821D4900's return value is used as a table
  index downstream.
- via `821CFC48 -> ... -> 821CECC0 -> 821464B8 -> bctrl`: crashes reading
  guest `0xCADA8000` (uninitialized memory) as an object. 821CECC0 walks a
  linked list and 82146690 looks the index up in table `0x82C6C3D8`, whose
  image shows a single entry (`count=1`); index 4 is out of bounds.

Common root candidate: the resource/index tables these paths read are
underpopulated because the platform table (second table, count 0) and the
streaming pool (null) were never built. Every downstream computation reads
zeros or stale heap and takes garbage branches. A probe that dereferenced
the release object faulted inside the probe itself and was fixed to log
registers only.

Still open: what `sub_821D4900` returns (its body is spin-loop heavy), and
whether index 4 / 0x14A0 would be valid with populated tables.

## Loader completes records; only record 0 never starts (measured)

Earlier probes read `record[i].state` as a byte, which is always 0. It is a
big-endian u32 at `0x831D5498 + i*400 + 8`. Read correctly, one boot sample of
records 0..3 and the current index gives:

```
st=[1 2 0 3] idx=0
```

and it does not change for the whole run.

Hardware watchpoints on the state words of records 1 and 3 show **one write
each, both at boot, both from `sub_82145968` via `sub_82145420`**:

```
REC1 0 -> 0x02000000  (state 2)   sub_82145968 <- sub_82145420 <- sub_82140000
REC3 0 -> 0x03000000  (state 3)   same chain
```

So `[1 2 0 3]` are **template defaults written once at boot**, not load
progress. Record 3's "3" is not a completion; nothing ever writes a record
state again in the whole run.

Corrected picture: the record array is initialised from a per-type template
and then never driven. The loader issues I/O (`sub_826793F0` x500, 4300+
AsyncIO completions) but no completion handler ever updates the record state
machine, so `record[0].state` stays at its template value 1 and the main
thread waits forever.

Two earlier claims are now retired: "the loader completes records" (wrong -
it was the template) and "record-state is a byte" (wrong - big-endian u32).

## Loads complete; the completion is never consumed (measured)

`sub_82678CD8` is the callback the title load issue path hands to
`pgStreamer::submit`. It does exactly one thing: `NtReleaseSemaphore` on the
per-slot handle at `[slot+12]`. It **fires** (hundreds of times per run):

```
completion 82678CD8 #1  slot=0xDB56D2C4 buf=0xCA7AE000 sem=0xF8001014
completion 82678CD8 #3  slot=0xDB56D8E4 buf=0xCADB0000 sem=0xF8001018
```

and the buffers it returns (`0xCA7AE000`, `0xCADA8000`, ...) are the very
pointers the release path later treats as objects, so that crash is on real
load buffers, not garbage.

The signal goes nowhere. For those handles in the same run:

```
sem 0xF8001014: waits=0  releases=1
sem 0xF8001018: waits=0  releases=1
```

No thread ever waits on a per-slot completion semaphore, so no completion is
ever processed, no record state is ever advanced, and the boot wait never ends.

The only thread in the image that waits on a loader-style semaphore is
`sub_82299490` (thread entry #13), and it waits on a single global handle at
`[0x82B3...]+7112`, not on the per-slot ones.

That is the wall in one line: **the loader issues and completes work, but the
consumer that drains per-slot completions does not exist in this run.**

## Work-event pulse does not unblock the producer (measured, reverted)

The streaming producer thread (`sub_821909D0`, entry #13 in the thread list)
parks on the guest work event `0x831D52F0`. The waker `sub_821904B8` never runs;
it is reached only via `sub_82190488`, which is stored in the vtable
`0x820BE4C0` (slot 0) and has no direct caller.

Pulsing that event from the host at ~60 Hz (`xeKeSetEvent`, labelled
`--experiment-wake-producer`) produced **no change**: `ready` stayed 0/1 as
before, `loadstate=1`, `presents=2 draws=6`, registry unchanged. The workaround
was removed per policy.

So the producer is not blocked *only* on that event, or it wakes and finds no
work it can act on. The job-queue object behind `[0x831D53EC]` was dumped as
well; its first word changes from the vtable pointer to `1` between samples and
no queue counters ever move.

## Session summary: what is proven, what is left

Proven with the two flags (`--experiment-clear-ready --experiment-frame-signal`):

- the stage machine reaches stage 7 and `sub_82141F00` runs the registry chain
  (`reg != 0`, `sub_822D3190` returns the manager);
- requests are produced (`sub_825120E8` x500+), the manager's update slots 5, 6
  and 11 run, and the title load issue `sub_826793F0` runs (ret=1);
- `pgStreamer` opens assets (`stream:\NNNNN`) and its worker completes
  thousands of IOs; the title completion callback `sub_82678CD8` fires and
  releases a per-slot semaphore;
- audio initialises and the title theme plays.

Not present anywhere in a run:

- any thread waiting on a per-slot completion semaphore (`waits=0`,
  `releases>0`), so completions are never consumed;
- any write to a record state after boot (watchpoints on records 1 and 3 show
  one write each, both from the boot template);
- the work-event waker `sub_821904B8` (pulsing the event by hand changed
  nothing);
- slots 7 and 8 dispatch, the streaming pool allocation, the second-table
  appender.

So the boot is not blocked by a missing flag, a wrong address, a driver, or a
race: it is blocked because **no consumer drains the loader completions**, and
the record state machine that the boot waits on is driven only by that consumer.

Unblocking requires either (a) finding and restoring that consumer, or
(b) explicitly driving the record state machine from the port - a labelled
workaround that fabricates guest progress, which the project rules allow only
with a named root cause and removal condition.

## The consumer exists and wakes, but finds nothing to do (measured)

The streaming consumer threads do exist: three threads start with entry
`sub_82798BA0` and park inside `sub_82798038`. Probing that function gives:

```
obj=0x82FEEC9C  h36=0x82FEECCC  h40=0  h4140=0xF8000848
```

so they wait on handle `0xF8000848` (which appears in the parked-handle list).
Signalling that object from the host (`--experiment-wake-consumer`, event or
semaphore, ~60 Hz for the whole run) produced 12 successful signals and **no
change**: record states stayed `[1 2 0 3]`, `loadstate=1`, `presents=2
draws=6`. The workaround is reverted.

So the consumer is present and wakeable; it wakes and finds no work it can
process. Combined with the earlier result that the completion callback fires
and releases per-slot semaphores that nothing waits on, the picture is:

- loads are issued and completed;
- completions are signalled on per-slot objects nobody waits on;
- the consumer waits on a different object and, when woken, does not advance
  any record.

This is no longer "a missing thread" - it is a mismatch between who is
signalled and who consumes, in the guest's own state machine.

## The work event IS signalled (measured)

`sub_821910D0` (called from the audio callback `sub_8219A2B8`) does
`KeSetEvent(0x831D52F0)` and **runs** (8 times in a 60 s run, returns each
time). `sub_82191DF0` would signal `0x831D5310` and has no caller.
`sub_821909D0` is not a waiter at all: it releases a semaphore and sets an
event on completion - it is a producer.

So the earlier assumption that "the work event is never signalled" is wrong: it
is signalled by the audio path and by the producer itself. Neither the guest
signal nor a host pulse wakes the parked consumer, and the consumer's queue
stays empty.

Remaining candidates for the queue-population gap, both static work:

- `sub_8218FFB0` / `sub_82191228`, called by the producer when its own work
  pointer is null (`loc_82190A64`), are the only paths that could enqueue;
- `825120E8` (registry request insert) runs 500+ times but its decompile is
  blocked by the custom prologue at `829FF7B8`, so its append target is still
  unread.

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
