# AGENTS.md

Standing rules for AI agents and contributors on the Linux port (`linux-pc` branch).
Task-specific instructions are not here; they are given in the session prompt.
Fill in every `TODO` before relying on this file.

## Languages

- **Existing code is C++ and stays C++.** Treat it as debt. Do not refactor it or port it to Zig. Touch it only for bug fixes, conflict resolution, or the small hooks needed to call Zig.
- **New code is Zig 0.16.0.** Pin this version. Check the 0.16 release notes before using std or build APIs from memory.
- Zig code lives in `zig/` and builds as a static library with a **plain C ABI**.
  - No allocators, slices, error unions, or STL types cross the boundary. Use pointers, lengths, ints, and status codes.
  - Export with `export fn` and a matching C header.
  - Avoid libc/libstdc++ dependencies where possible.
- CMake runs `zig build-obj` (or `zig build-lib`) and links the result into `LibertyRecomp`.

## Repo map

- Third-party rules exist in `glue/rexglue-sdk-main/thirdparty/sdl3/AGENTS.md` and `o1heap/AGENTS.md`. They do not apply to this repo.
- Port-relevant files: `video.cpp`, `main.cpp`, `createdevice_diag.cpp`, `draw_pool_watchpoint.cpp`.
- Generated, never edit or read in full: `gta4_recomp.86.cpp`.
- Memory-safety workarounds belong in `kernel/memory`.
- Upstream: `elripalda/main`.

## Git rules

- Before any history rewrite, create a backup branch (`backup/linux-pc-YYYYMMDD`).
- Never rebase with a dirty tree. Commit work-in-progress to a `wip/` branch first.
- Record submodule SHAs before and after any rebase or merge. Never mix submodule pointer changes with code changes.
- Build fixes and runtime fixes go in separate commits.
- Do not introduce Zig in the same commit series as a rebase.
- Never force-push unless asked.
- Small, reviewable commits with the reason in the message.

## Workaround policy

A workaround hides a bug; it does not fix it.

- Label it `WORKAROUND` in code and in the commit message.
- It must log every trigger (file, address, size). A silent workaround is a bug.
- Do not extend or copy a workaround into new code, and do not port one to Zig before its root cause is understood.

## Zig rules (Zig 0.16.0)

### Toolchain
- Never write std APIs from memory. Check the installed std source (`zig env`, field `std_dir`) first. Training data is mostly pre-0.16.
- 0.16 differences: I/O needs a `std.Io` instance; containers are unmanaged (pass the allocator per call); `@Type` is gone; use `addTranslateC` instead of `@cImport`.
- After every Zig change run `zig fmt .`, `zig build`, and `zig build test` in `zig/`. Do not say "done" until all pass. Also run the C++/CMake build if the change affects linking.

### Style
- Types PascalCase, functions camelCase, everything else snake_case.
- Short functions, early returns, no deep nesting.
- Comments explain WHY (cite the spec, the commit, or the bug), not what.
- Named constants instead of magic numbers.
- Keep module boundaries clean: memory/kernel code never imports graphics, and graphics code never imports kernel internals. Layers: `LibertyRecomp/cpu`, `LibertyRecomp/gpu`, `LibertyRecomp/kernel`, `LibertyRecomp/os`, `LibertyRecomp/runtime`, `LibertyRecomp/patches`, `LibertyRecomp/ui`, `LibertyRecomp/install`, `LibertyRecompLib` (recompiled game, generated).

### Guest/runtime correctness
- Use fixed-width ints (`u8`..`u64`) for guest values, never `usize`.
- Use wrapping ops (`+%`, `-%`, `*%`) where the original hardware or code wraps.
- Hardware and ABI layouts: `extern` or `packed struct` plus comptime size assertions.
- Guest addresses and host pointers are different types. Convert only at one named place.
- Comment every `@intCast`, `@ptrCast`, and `@bitCast` with why it is safe.
- Guest input is untrusted: bounds-check it and report faults through the project's diagnostics. Never panic or crash the host.

### Memory and errors
- No hidden allocation: take a `std.mem.Allocator`, and put `defer`/`errdefer` on the line right after the acquire.
- No allocating, logging, or formatting in hot paths.
- Explicit error sets, no `anyerror`.
- No `catch unreachable`, `@panic`, or `orelse unreachable` for anything the guest or the environment can trigger.

### C interop (Vulkan, SDL, existing C++ code)
- Wrap each C library in one Zig module. Convert C errors and pointers at that boundary.
- Every object has one owner and a `deinit`, called in reverse creation order.

### Tests
- Put `test` blocks beside the code, using `std.testing.allocator`. Cover bad input and edge cases.

## Correctness: do not hide bugs

- Fix the root cause. If you cannot, add a workaround that follows the workaround policy above and open a note for the root cause.
- Fail loudly. Do not swallow errors, add silent fallbacks, clamp or zero bad values without logging, or widen a guard to make a crash go away.
- Do not "fix" a crash by committing more memory, skipping the draw, or ignoring a null. First find out why it is null or uninitialized.
- Make bugs reproducible before changing code: a test, a log line, or exact run steps.
- Never delete or weaken a test, assert, or log to get a green result.
- Report "I could not verify this" instead of guessing.

## Build and test

Run all commands with `LC_ALL=C` so output is parseable (the dev machine uses a German locale).

- Target: openSUSE Tumbleweed-Slowroll 20261001, kernel 7.2.11-1-default, AMD RX 580 (Polaris10, Ellesmere rev e7), Mesa 26.2.5 RADV (`AMD Radeon RX 580 Series (RADV POLARIS10)`), Vulkan instance 1.4.363, Brew Clang 23.1.3 (`/home/linuxbrew/.linuxbrew/bin/clang`), Ninja, ccache, CMake 4.4.4.
- Configure: clean build dir (e.g. `build-linux/`; do not reuse stale `build-clang/`). Fresh configure from repo root with Ninja, Release, `LIBERTY_RECOMP_ARCH=x86-64-v3`, `LIBERTY_RECOMP_OPTIMIZE_TOOLS=OFF` (ON emits thin-LTO bitcode into tools libs that GNU ld cannot link into `librexruntime.so`), `CMAKE_CXX_SCAN_FOR_MODULES=OFF` (system `clang-scan-deps-23` defaults to `x86_64-suse-linux` and rejects the Brew-Clang PCH built for `x86_64-unknown-linux-gnu`), Brew Clang on PATH, vcpkg-free system/brew deps, ccache default (`CMAKE_C_COMPILER_LAUNCHER`/`CMAKE_CXX_COMPILER_LAUNCHER=ccache`, already default in prior `build-clang/` cache). Upstream preset alternative `cmake --preset linux-release` uses vcpkg + `toolchains/linux-clang.cmake` and diverges; prefer explicit clean dir above for the Linux port.
- Build: from repo root `cmake --build build-linux --target LibertyRecomp` (verify binary path after build).
- Run: launch the built `LibertyRecomp` binary with own Xbox 360 files via app installer; no game/firmware/keys in repo.
- Zig: `zig version` must print `0.16.0`, then `zig fmt .`, `zig build`, `zig build test` in `zig/`.

## Definition of Done

Linux is "working" when all of these hold on the target above:

1. Boots to a visible recognizable game start: intro, start screen, start menu, or main menu. No crash.
2. Reaches in-game is out of scope for this DoD (menu visible is the bar).
3. Performance/visual threshold: recognizable frame; no fps number required.
4. No workaround triggers silently

## Agent behavior

- Ask at most one question at a time, and only when blocked.
- At the end of each task, state what you skipped and what risk that leaves.
- Do not paste or rewrite generated files.
- If a change touches a file that upstream recently moved, say so explicitly.
- Do not claim something builds or runs unless you ran it.
- Small focused diffs. No new dependencies without asking. Match the surrounding style.
- Never add game files, firmware, keys, or extracted assets to the repo.
- If you cannot verify an API or the requirement is unclear, say so and ask.
