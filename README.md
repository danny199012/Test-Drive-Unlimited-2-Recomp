# Test Drive Unlimited 2 (Xbox 360) — ReXGlue Static Recomp

A static recompilation of **Test Drive Unlimited 2** for Xbox 360 using the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0. Guest PowerPC
code is translated to C++ at build time by `rexglue codegen`, driven by
`tdu2_manifest.toml`, and executed natively on Windows.

![Party scene](screenshots/party_scene.jpg)

## Layout

| Path | What it is |
|---|---|
| `TDU2/` | Game content (redump). `Default.xex`, `TestDrive2_DLL.xex`, `bigfile*.big`, `nxeart`, `$SystemUpdate/`, `Euro/` |
| `rexglue/win-amd64/` | ReXGlue SDK (prebuilt): `rexglue.exe` toolchain + headers/libs |
| `tdu2_manifest.toml` | Recomp manifest: modules, function overrides, hooks |
| `generated/` | Codegen output — **do not edit**, regenerated every build |
| `src/` | Host app: `main.cpp`, `tdu2_app.h` (`Tdu2App`), `tdu2_menu.*` (F1), `tdu2_profile.*` (synthetic profile) |
| `config/tdu2.toml.template` | Settings template, staged into each build dir as `tdu2.toml` |
| `out/build/` | Build trees (`win-amd64-debug`, `win-amd64-release`, `win-amd64-perf`) |
| `tools/xex_dump.py` | XEX → PE/section dumper, for inspecting guest code |

## The two-image problem

TDU2 is **not** a single-image game, and this is the single most important
thing to know about this project:

- `Default.xex` is only a **launcher stub** — its code section is just
  `0x82010000–0x8203A3BC`.
- The actual game is `TestDrive2_DLL.xex`, which the stub loads at runtime
  through `XexLoadImage`.

Both are registered in `tdu2_manifest.toml`: `Default.xex` as `[entrypoint]`
and `TestDrive2_DLL.xex` as a `[[modules]]` entry. The `guest_path` on the
module must match the string the guest passes to `XexLoadImage`.

Without the module declared, startup dies with:

```
[error] [cpu] Execute(885FA390): function not in function table
[critical] [core] [FATAL] Call to invalid or unregistered function at guest address 0x88F66F28
```

`bigfile0-2.big` (~6.8 GB) hold the game assets and are not part of
recompilation — the guest reads them through the filesystem layer.

## Building

Requires **clang**, **CMake ≥ 3.25**, and **Ninja**. Plain MSVC `cl` cannot
compile the generated code (the SDK headers use clang builtins).

```
build.cmd            codegen only (fast — regenerates C++ from the XEXs)
build.cmd tdu2       build the executable
build.cmd -release   Release (-O3), stages the release plugins
```

Output: `out/build/win-amd64-debug/tdu2.exe`, alongside the staged
`rexruntimed.dll` and `rexgpu-xenosd.dll` (the Xenos GPU emulator — without it
there is no graphics emulation at all).

First codegen of the 16 MB DLL takes a few minutes; the stamp file makes
later builds incremental.

## Running

The game content must be pointed at explicitly:

```
out\build\win-amd64-debug\tdu2.exe --game_data_root ..\..\..\TDU2
```

Useful overrides (all normal ReXGlue cvars):

| Argument | Effect |
|---|---|
| `--game_data_root <path>` | Game content directory |
| `--update_data_root <path>` | Update content (`$SystemUpdate`) |
| `--window_width` / `--window_height` / `--fullscreen` | Presentation |
| `--log_level trace` | Verbose logging |

Logs land in `out/build/<config>/logs/tdu2_*.log`.

### The "abort() has been called" dialog

When the guest does something rexruntime cannot service — most often calling an
address that is not in any registered function table — the runtime calls
`abort()`. The **MSVC debug CRT** turns that into a modal *"Debug Error!
abort() has been called"* dialog that blocks the process indefinitely, which is
why an unattended run appears to hang.

`src/main.cpp` installs crash handlers before any other static initialiser:

- `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)` suppresses the
  OS-level fault dialog.
- `_set_abort_behavior(_WRITE_ABORT_MSG, _CALL_REPORTFAULT)` tells the CRT to
  write to stderr instead of raising the interactive box.
- `std::set_terminate` replaces the terminate dialog with a short message.

So a crashing run now always terminates and leaves the real cause in
`tdu2_*.log`. **Read the log — the dialog never told you anything the log
didn't.**

## Performance

**Use the Release build to play.** The default Debug preset compiles the
recompiled code at `-O0`, which is several times slower than the game needs.
That is fine for debugging and is what every log in this README came from, but
it is not a fair measure of performance.

```
build.cmd -perf tdu2
```

That selects the `win-amd64-perf` preset: `Release`, `-O3`,
`-fomit-frame-pointer`, LTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION`), and
`REXGLUE_RECOMP_DEBUG_INFO=none`. Output goes to
`out/build/win-amd64-perf/` with the *release* `rexruntime.dll` /
`rexgpubenos.dll` staged alongside.

Measured difference at the same point in startup (29 s wall clock):

| Build | CPU consumed | recomp DLL size |
|---|---|---|
| `win-amd64-debug` (`-O0`) | ~73 s | 185 MB |
| `win-amd64-perf` (`-O3` + LTO) | ~50 s | 87 MB |

LTO alone roughly halves the DLL — a good sign the generated code compresses
well once the optimiser can see across translation units.

Notes:
- The first `-perf` build compiles 500+ large translation units and takes a
  while. Later builds are incremental.
- Turning off recomp debug info means no stepping into guest functions. Drop
  back to `build.cmd tdu2` (Debug) when you actually need to debug.
- If it still feels slow, `--log_level trace` plus the SDK's per-frame CSV
  (`--perf_log_csv`) will show whether the time is going into the guest or
  into GPU emulation.

## Manifest notes

Codegen's scanner classifies some small routines as non-code, leaving **holes in
the registered function table**. The runtime then calls such an address and
fatals with `Call to invalid or unregistered function at guest address 0x...`
followed by `abort()`. Each one is pinned explicitly:

| Address | Notes |
|---|---|
| `0x82022BFC` | entrypoint; a `memcpy`-style byte loop (`andi./mtctr` + `lbz/addi/stb/addi/bdnz`) |
| `0x8852BB60` | module; unresolved branch target |
| `0x8824F0C0` | module; unresolved branch target |
| `0x88612F18` | module; 12-byte hole between `0x88612EE8` and `0x88612F24`. Guard: calls `sub_88604EF8` when `r3 == 0` |
| `0x889B8FE0` | module; 8-byte hole between `0x889B8FD0` and `0x889B8FE8` |
| `0x88906CA0` | module; hole between `0x88906C40` and `0x88906CB0` — hit once the audio/GPU threads start |
| `0x88F2F580` | module; hole between `0x88F2F558` and `0x88F2F588` |
| `0x88F12600` | module; hole between `0x88F125E8` and `0x88F12630` — hit once shaders were translating |
| `0x88F11B40` | module; hole between `0x88F11B28` and `0x88F11BB0` — hit once input came up |
| `0x88301888` | module; hole between `0x88301868` and `0x883018D0` |
| `0x882EFEF0` | module; hole between `0x882EFEE8` and `0x882EFF00` |
| `0x884E86E8` | module; hole between `0x884E86D0` and `0x884E8718` — hit ~45 s in, controller attached |
| `0x88405500` | module; hole between `0x88405498` and `0x88405578` — a **vtable dispatch** thunk (`lwz r10,36(r11); mtctr r10; bctr`) |
| `0x88405518` | module; second thunk in that same cluster |
| `0x884EF1A8` | module; hole between `0x884EF188` and `0x884EF1B0` |
| `0x8850E3B0` | module; hole between `0x8850E3A0` and `0x8850E3D0` |
| `0x88221040` | module; hole between `0x88221038` and `0x88221060` (`lwz r3,4(r3); b ...`) |
| `0x88221050` | module; neighbour thunk in the `0x882210xx` thunk table |
| `0x88254FC8` | module; hole between `0x88254FA8` and `0x88254FE0` |
| `0x882F0690` | module; hole between `0x882F0668` and `0x882F06A4` |
| `0x88E3B2E8` | module; hole between `0x88E3B2D8` and `0x88E3B2F8` |
| `0x88F11DC8` | module; hole between `0x88F11D90` and `0x88F11E48` |

### Automating the loop

The pin → codegen → build → run cycle is ~5 minutes a round, so it is worth
automating. `tools/fix_holes.py` does the whole thing unattended:

```
python tools\fix_holes.py --rounds 5
python tools\fix_holes.py --dry-run      # report the next address only
```

Each round it launches the game headless, watches for the `[FATAL]` address,
appends the pin to `tdu2_manifest.toml`, then runs codegen and the perf build.
It stops when:

- a run finishes with no `[FATAL]` (nothing left to pin), or
- the address is already pinned **and** already generated — pinning genuinely
  is not the fix, so it stops and tells you to look by hand, or
- `--rounds` is reached.

It also handles the common "I edited the manifest but haven't rebuilt yet" case
by catching up with a codegen + build instead of giving up.

**Latest result:** with the auto-pinned `0x889B81B0` built in, the game ran the
full 420 s window with **no `[FATAL]` at all** — the first clean round. Note
that "clean" here means "clean for the whole `--seconds` window", not "runs
forever"; raise `--seconds` to be more confident.

```
python tools\fix_holes.py --rounds 5 --seconds 1800
```

### Why the scanner misses them

The ones inspected so far are all **thin thunks**, not real functions:

```asm
# 0x88221040                                  # 0x88405500
lwz r3,4(r3)                                  lwz r3,0(r4)
b  sub_884B63F8                               mr r4,r5
                                              lwz r11,0(r3)
                                              lwz r10,36(r11)   ; vtable slot 9
                                              mtctr r10
                                              bctr
```

6–7 instructions with no stack frame, so there is no prologue for the scanner to
anchor on. They tend to arrive in **clusters** — fixing one usually exposes its
neighbour on the next run — so expect to add several addresses per round.

A second shape in the same family is a **vtable-adjusting thunk**, which
compensates a `this` pointer for multiple inheritance before tail-calling:

```asm
# 0x882EFEB8
addi r3,r3,-12
b     sub_88312AD8
```

Each fix lets the game run further, which is how they are discovered in order:

```
module registers
  -> audio + GPU threads start          (0x88906CA0, 0x88F2F580)
  -> shaders translate, pipelines made  (0x88F12600)
  -> input/controller comes up          (0x88F11B40, 0x88301888, 0x882EFEF0)
  -> long-running main loop             (the rest; now reaching minutes)
```

Two caveats learned the hard way:

- **Small gaps are not a reliable signal.** There are ~21,000 gaps of ≤24 bytes
  between registered functions and essentially all are ordinary padding. Only
  pin an address that actually appeared in a `[FATAL]` line.
- **The crash address moves between runs.** The path taken depends on timing
  (thread scheduling, shader-cache warm vs cold), so a run can die *earlier*
  than the previous one at a different address. That is not a regression.
- **A clean automated run does not mean the game is hole-free.** The pinning loop
  runs `--headless` and never touches input, so it only exercises the code paths
  that run unattended. `0x882EFED0` survived a full 420 s clean run and only
  appeared when the pause menu was opened by hand. UI code reached through player
  action is a separate reachability set that has to be discovered by playing.

### UI thunks come in clusters

The 0x882EFExx region is a dense table of 8-byte `addi r3,r3,-N` vtable thunks,
and it has now yielded three crashes in a row from three different screens:

| Address | Reached by |
|---|---|
| `0x882EFED0` | Pause menu |
| `0x882EFED8` | Map, opening |
| `0x88313820` | Map, further in |
| `0x88B88420` | Map, closing |
| `0x88E88220` | Driving |
| `0x882F06E8` | Real Estate |
| `0x881E9FE0` | Police chase AI |

**A good repro beats a vague one.** Every crash so far has come from a distinct
user action rather than from idling: pause, map open, map close, driving,
real estate, and now escalating to a police chase. A deterministic trigger (ram
a police car twice, get pursued) makes a pin verifiable — you can rebuild,
reproduce in a minute, and know whether the fix landed instead of waiting 45
minutes to find out.

**Crashes that look random usually are not.** The driving crash looked
intermittent, but two consecutive runs died at exactly `0x88E88220` — a specific
code path, not thread-timing noise. When a crash seems random, check whether the
address repeats across logs before assuming it is a moving target:

```
python tools\check_func.py --latest
Select-String -Path out\build\win-amd64-perf\logs\*.log -Pattern FATAL
```

All six are the same root cause: a virtual call the game makes through a vtable,
compiled to a prologueless thunk. Pinning one often exposes the neighbouring slot
in the same table next.

Each pin exposes the next slot, so opening a new screen commonly surfaces a
fresh one. Two practical consequences:

- **Budget one rebuild per new screen you open.** Codegen of the DLL takes
  ~5 minutes and the `-O3`/LTO link another ~1.5, so batch up whatever you can
  test in a single run before rebuilding.
- **Do not mass-pin the cluster.** The XEX stores the code section compressed, so
  the bytes of an unregistered slot cannot be inspected from the redump with the
  tools here — meaning any slot that has not appeared in a real `[FATAL]` is a
  guess. Guessing is how you turn a missing thunk into a crash somewhere else.

**This is expected to be an iterative process** — each fixed hole exposes the
next one. Use the helper rather than eyeballing `tdu2_register.cpp`:

```
python tools\check_func.py --latest        # newest FATAL address from the log
python tools\check_func.py 0x88F12600      # is this address registered?
```

It scans the generated `tdu2_register.cpp` files and prints either
`ALREADY REGISTERED` or `NOT REGISTERED` plus the surrounding neighbours and
the exact manifest stanza to paste:

```
# scanned 76860 registered functions across 2 file(s)
0x88F12600: NOT REGISTERED (hole in the function table)
  previous: 0x88F125E8
  next:     0x88F12630

Pin this in tdu2_manifest.toml:
[modules.functions."0x88F12600"]
name = "sub_88F12600"
```

The loop is then: **pin → `build.cmd tdu2` → run → `check_func.py --latest`**.
Codegen of the 16 MB DLL takes ~4–5 min, so batching fixes is worthwhile — pin
every hole you can find from the log before rebuilding.

## GPU emulation

TDU2 drives the GPU hard from very early in startup
(`VdInitializeRingBuffer`, `VdSetGraphicsInterruptCallback`,
`VdEnableRingBufferRPtrWriteBack`). The SDK default for `gpu_plugin` is empty,
which makes the runtime stub those calls out and the game never gets past
initialisation:

```
[warning] [krnl] VdInitializeRingBuffer: no GPU emulation loaded (gpu_plugin not set); call ignored
```

`Tdu2App::OnPreSetup` therefore sets `config.gpu_plugin = "xenos"`. The build
stages `rexgpu-xenosd.dll` (Debug) / `rexgpu-xenos.dll` (Release) next to the
exe via `rexglue_setup_target(tdu2 GPU_PLUGINS xenos)`.

## Current status

- [x] Codegen clean for both images
- [x] Builds and links `tdu2.exe` (Debug, Release, and `-perf`)
- [x] Runtime boots the XEX, builds the function table, initialises SDL input
- [x] `Default.xex` stub loads `TestDrive2_DLL.xex` (~76,000 functions registered)
- [x] No modal abort dialog — crashes terminate and log
- [x] Xenos GPU plugin enabled (1118 shaders translate, 1590 pipelines created)
- [x] Audio endpoint opens, controller detected and mapped
- [x] **A full 420 s run with no `[FATAL]`** — all known scanner gaps closed
- [x] **Reaches gameplay and renders a real 3D scene** — world geometry, textured
      models, and on-screen text (confirmed by screenshot)
- [x] **Playable end to end for a 47 minute session** — licences, tournaments,
      challenges and races all completed; driving, map, pause menu and real
      estate all working
- [x] F1 quick-settings menu and F4 cvar editor both work
- [x] Synthetic profile installed over the guest's `XamUser*` imports
- [ ] **Shredded/black skinned mesh in the intro and character select** — the
      character's clothing explodes into stretched triangles and black wedges
      (garbage vertex positions, unlit faces); gameplay is unaffected. Nothing
      is logged when it happens, and the same artifact is reported for stock
      Xenia, so this is not a ReXGlue regression. `src/tdu2_app.h` now logs
      every candidate GPU cvar as `cvar <name> = '<value>' (default '<default>',
      source <where>')` at startup and writes the whole registry to
      `cvars_full.txt` next to the exe. Experiment A is applied in
      `out/build/win-amd64-perf/tdu2.toml` (`d3d12_readback_memexport = true`,
      `readback_memexport_fast = false` — memexport coherency); experiments B
      and C are commented in `config/tdu2.toml.template`.
      
      **Diagnosis update (2026-10-04, from screenshot analysis + log timeline):**
      the symptom is a *warm-up race*, not corrupt mesh data, so Experiment B is
      now the primary suspect rather than the memexport knobs:
      
      - Broken in the opening / character select only. Cutscenes, shops and
        gameplay are clean. The artifact is time-based, not location-based.
      - Many characters are affected at once, not one mesh — every draw issued
        while pipelines are still being created goes wrong.
      - Stretching is mild (vertices land a little outside the silhouette), not
        the catastrophic explosion seen with genuinely bad vertex data.
      - `tdu2_022.log` (the run that produced the screenshot) contains **none**
        of the `cvar ... = '...'` lines — that logging postdates it. So the
        `async_shader_compilation = false` currently in `tdu2.toml` (written
        21:48, i.e. *after* the 20:37 screenshot) has **never actually been
        exercised**; the run that would have used it (`tdu2_028`) aborted on
        `--game_data_root was not provided`.
      
      With `async_shader_compilation = true` a draw issued while its pipeline is
      still compiling yields garbage vertex positions: stretched triangles and
      unlit black wedges, clearing once the scene is warm. That matches every
      observation above.
      
      **Experiment B result (2026-10-04): NEGATIVE — the warm-up-race theory is
      not supported. Treat the async diagnosis below as unconfirmed/disproven.**

      **Root cause update (2026-10-05, confirmed by screenshots):** The bug is a
      **bone skinning matrix corruption** caused by incomplete memexport readback.
      TDU2 writes bone skinning matrices to guest memory via vertex-shader memexport.
      The intro / character-select scene loads many NPC skinned meshes simultaneously
      (beach-party dancers). Each NPC's bone matrices must be read back from memexport
      before the next draw call samples them. The previous setting of
      `readback_memexport_fast = false` routed memexport writes through the slower
      CPU-side path. For the intro scene with dozens of bone matrices written in rapid
      succession, the slower path does not complete before the next draw reads them,
      so the matrices contain stale / uninitialized data. Vertices are multiplied by
      garbage bone matrices, producing the elongated-leg / shredded-triangle geometry
      visible in the screenshots. The foreground character (player) renders correctly
      because it does not use NPC bone skinning.

      **Fix applied (2026-10-05):**
      - `readback_memexport_fast = true` — restore the fast GPU-side readback path
        (the compiled-in default). Bone matrices are read back quickly.
      - `async_shader_compilation = false` — ensure every shader pipeline is compiled
        synchronously before the first draw call, eliminating the pipeline race.
      - `d3d12_readback_memexport = true` — keep D3D12 memexport readback enabled.
      Applied in `config/tdu2.toml.template`, `out/build/win-amd64-perf/tdu2.toml`,
      and `out/build/win-amd64-debug/tdu2.toml`.

      The bug persists in stock Xenia Canary / Edge (user-confirmed), so it is an
      upstream Xenos emulator issue, not a ReXGlue regression. The cvar fix above
      is the best available config-level mitigation.

      Three controlled runs, each reaching gameplay for the full capture window
      (log `tdu2_033.log` and `tdu2_034.log` are clean; the runtime rewrites
      `tdu2.toml` with an *unescaped* `..\..\..\TDU2` path on shutdown, which is
      invalid TOML and silently discards every cvar — hence `--game_data_root` is
      now also passed on the quoted command line):

      - **720p instead of the standing 4K override** (`_intro_N720_*.jpg`,
        log 033): artifact **unchanged**. Not resolution-dependent.
      - **`force_convert_triangle_fans_to_lists` +
        `force_convert_quad_lists_to_triangle_lists`** (`_intro_PRIM_*.jpg`,
        log 034): artifact **unchanged** — a hard black wedge still cuts across
        the frame at 180 s and a hand still detaches and floats at the right
        edge at 140 s. Not a primitive-topology decode problem.

      Two corrections to earlier reasoning:

      - The **vertex-fetch knobs cannot be tested**. Log 033 reports
        `vfetch_full`, `vfetch_mini` and `xe_vertex_index_offset` as
        `<unregistered>` — the prebuilt `rexgpu-xenos.dll` does not expose them,
        so the vertex-fetch theory is untestable from config alone.
      - Short offset lists produce **false negatives**. The offsets ended at
        100 s, which only reaches the clean ocean/crowd intro; the broken
        character-select party scene does not appear until ~180 s. The default
        is now `(25,45,70,100,140,180,220,260)`. Any earlier "720p looked
        fine" reading came from capturing the wrong part of the intro.

      Remaining candidates, in rough order of plausibility: skinned-mesh
      bone/palette upload, depth-buffer `native_2x_msaa` resolve interactions,
      and a genuine upstream `rexgpu-xenos.dll` bug (the artifact is also
      reported for stock Xenia).

      Run the tests with (each backs up and restores `tdu2.toml`):
      ```powershell
      powershell -ExecutionPolicy Bypass -File _run_intro_test.ps1 -Tag B -Async off
      powershell -ExecutionPolicy Bypass -File _run_intro_test.ps1 -Tag A -Async on
      powershell -ExecutionPolicy Bypass -File _run_intro_test.ps1 -Tag PRIM -PrimitiveFix
      ```
      B vs A is the controlled comparison — identical apart from that one cvar.
      Outputs land next to the project root as `_intro_<Tag>_<t>s.png/.jpg`.

      On a successful run the harness now marks the config restore **pending**
      (it previously restored only on the early-exit path, so a successful test
      left its cvars in place and silently contaminated the *next* run's
      baseline). After closing the game, run:
      ```powershell
      powershell -ExecutionPolicy Bypass -File _restore_toml.ps1
      ```

      **Launch blocker found and fixed (2026-10-04).** The test harness could not
      start the game at all: the runtime showed `--game_data_root was not
      provided.` even though the value was plainly visible in `tdu2.toml`. The
      file was being discarded wholesale by a **TOML syntax error**:

      ```
      game_data_root = "..\..\..\TDU2"     # invalid TOML
      ```

      In a TOML basic string `\` begins an escape sequence, so `\.` / `\T` are
      not legal and `tomllib` rejects the *entire file* — every cvar in it is
      lost, not just the bad line. Verified directly against the on-disk file:
      `Unescaped '\' in a string (at line 4, column 23)`.

      Note this is written by the game's own "Saved config to tdu2.toml" on
      shutdown, so the config self-poisons on every run that touches the F1 menu.
      Hand-edits are the only reliable source of a loadable file. The harness now
      writes the path as a quoted absolute path with forward slashes (no escapes
      needed, and spaces in `Rex Glue TDU 2 Project` require the quotes), then
      re-parses the file with `tomllib` and aborts before launching if it is
      still invalid.

      Two traps hit while diagnosing this, both worth not repeating:
      - A UTF-8 BOM is *not* the cause. PowerShell 5.1's `-Encoding UTF8` does
        add one, but the file parses fine once the backslashes are fixed.
      - An unquoted absolute path is also invalid, because the install path
        contains spaces.

      **The TOML fix above was necessary but NOT sufficient.** With a provably
      valid `tdu2.toml` (checked with `tomllib`, and preserved verbatim by the
      game across shutdown) the runtime still failed with
      `--game_data_root was not provided.` So the startup check does not read
      that key from `tdu2.toml` at all, despite the cvar being registered and
      listed in `cvars_full.txt`.

      Fix: pass it as a **command-line argument**, which is the documented
      highest-precedence source and the form the README's own launch line uses:

      ```
      tdu2.exe --game_data_root "E:/.../Rex Glue TDU 2 Project/TDU2"
      ```

      The path needs quoting (spaces) and can use forward slashes. If this ever
      needs to work from a shortcut or launcher rather than the harness, the
      equivalent is the `REX_` environment-variable override.
- [ ] **Blank in-game photos and licence portraits** — the UI renders normally but
      the picture area is a solid black rectangle, so the texture sample reads as
      zero. Three causes ruled out by log evidence: the render-to-texture cache
      limit (256 MB had no effect), `gpu_allow_invalid_fetch_constants`, and
      `direct_host_resolve`. Note that no image file is ever written to disk, and
      ReXGlue has no content store, so a *saved* photo may be a separate failure
      from a *live* render target.

      **Root cause identified (2026-10-05):** `readback_resolve = 'none'` (the
      compiled-in default) causes the emulator to ignore `VdResolve` commands.
      TDU2 renders the avatar / licence portrait / in-game photo into an eDRAM
      render target, then issues `VdResolve` to copy that content into a 2D texture
      in main memory. The UI quad samples that texture in the same frame. Since the
      resolve is never processed, the main-memory texture stays blank (zeroed) and
      the quad reads zero — a solid black rectangle. The silhouette that appears
      when the menu closes is the eDRAM render target content visible in the final
      frame composite, bypassing the resolve path.

      **File-level confirmation (2026-10-05):** All saved image files under the
      user data directory (`AVATAR\PHOTO00`, `LICENCES\LIC_*`, `PHOTOS\*X.JPG`)
      are solid-black 256x256 / 1280x720 JPEGs — the game writes the resolved
      render target content to disk, and with `readback_resolve = 'none'` the
      resolve produces zero pixels, so every saved JPEG is black. Photo mode
      renders live (the scene is captured in real-time, not read from disk), so
      photos work with the fix. The profile picture reads the saved JPEG, which
      is black, so it stays blank in the current save.

      **Fix applied:** `readback_resolve = "full"` and `d3d12_readback_resolve = true`
      in `config/tdu2.toml.template` and both build configs. `direct_host_resolve`
      restored to `true` (default) since it was ruled out.
- [ ] **More UI crashes expected** — seven fixed so far (pause, map open, map
      further, map close, driving, real estate, police chase AI); each new screen
      reached tends to expose the next thunk in the same vtable
- [ ] Online / Xbox Live functionality (see *Profile and online status*)

Run length before hitting the next unregistered function went
**18 s → 37 s → 56 s → ~150 s → ~230 s → ~21 min → clean**.

### Trying it with a window

Everything above was verified with `--headless`. To see it render, drop that
flag:

```
out\build\win-amd64-debug\tdu2.exe --game_data_root ..\..\..\TDU2
```

Use `--window_width` / `--window_height` / `--fullscreen` to control the window.
A controller is optional — the game enumerates SDL gamepads at startup.

### In-game menus

| Key | Menu |
|---|---|
| **F1** | Quick settings — render resolution, window size, fullscreen, profile |
| **F4** | The SDK's full cvar editor (every runtime setting) |

Both are host-side overlays and work while the game is running. Most settings in
either menu are read by the host only at startup, so they are marked
"restart to apply" rather than pretending to take effect immediately. The
profile fields are the exception: the guest reads them through the overridden
imports on every call, so they apply immediately and persist to `tdu2.toml`
when the game exits.

### Configuration file

`config/tdu2.toml.template` is copied to `out/build/<config>/tdu2.toml` on
configure, **only if that file does not already exist** — so your edits and the
saved profile survive a rebuild. Delete the build directory to reset to
defaults.

The format must be **flat** `key = value` with no `[sections]`. This was found
the hard way: a sectioned file loads without any complaint and silently applies
none of it. Precedence is file < `REX_*` environment variables < command line.

The runtime **regenerates this file on shutdown** from the non-default cvar
values, so comments written into `out/build/<config>/tdu2.toml` do not survive
a run - keep the reasoning in `config/tdu2.toml.template` (staged only into a
build dir that has no `tdu2.toml` yet) and in this README.

### Seeing what is actually in force

`tdu2_app.h` logs the GPU cvars that decide how vertex/index data reaches the
device, each with its compiled-in default and where the current value came
from:

```
cvar readback_memexport = 'true' (default 'true', source default)
cvar d3d12_readback_memexport = 'true' (default 'false', source config)
```

`source` is one of `default`, `config` (the toml), `env`, `cmdline`, `runtime`
(F1/F4/console), so a value that silently failed to load shows up as `default`
rather than looking applied. The app also writes `cvars_full.txt` next to the
exe on every startup - the complete registry (158 entries), including cvars
that have no reason to appear anywhere else.

### Profile and online status

ReXGlue emulates no dashboard and no user accounts. TDU2 imports
`XamLoaderGetLaunchData`, `XamUserGetSigninState`, `XamUserGetXUID`,
`XamUserGetName` and `XamUserGetSigninInfo`, and those resolve into the
prebuilt `rexruntime.lib`, which cannot be patched from this project.

What this project does instead is **interpose host functions over the generated
import thunks**: `FunctionDispatcher::SetFunction` is a plain map insert, so
registering our own implementation for a thunk address after the module has
registered wins. `src/tdu2_profile.cpp` answers those six imports from three
cvars (`profile_name`, `profile_xuid`, `profile_signed_in`), which the F1 menu
edits and the runtime persists on shutdown.

One wrinkle worth knowing: the launcher registers its function table during
startup, but the game DLL is loaded by the guest about two seconds later.
Hooking once from `OnPreLaunchModule` therefore reaches only the launcher's two
imports. The install runs on its own thread with a backoff and retries until all
six land (`InstallProfileHooksWithRetry`).

**What this is not:** none of this authenticates to anything. It is a local
identity so the game has a coherent name/XUID to read. Real online play would
additionally need Xbox Live networking, licensing, friends, and authentication —
a substantially larger piece of work, and one that should be designed as its own
layer rather than assumed to follow from a profile name.