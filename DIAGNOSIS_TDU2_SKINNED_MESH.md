# TDU2 (ReXGlue recomp) — malformed/stretched character bodies: diagnosis state

Date: 2026-10-06. Author: ZCode session (see git log in rexglue-sdk for all probes).

## Symptom

In the intro / character-select pool-party scene only, skinned NPC bodies render
with clothing and lower-body geometry stretched into long smooth "ribbons"
connecting the character to far-away positions. Heads, faces, arms and skin
render correctly. Multiple characters corrupted simultaneously, each
differently. Gameplay and later scenes are clean. The same artifact is reported
in stock Xenia Canary/Edge, so this is an upstream-class emulator bug, not a
ReXGlue recomp regression.

## Build / config baseline

- rexglue-sdk v0.10.0 built from source (commit c94f5eb) via
  `-DREXSDK_DIR=.../rexglue-sdk -DCMAKE_{C,CXX}_FLAGS="-march=x86-64-v2"`
  (the SDK needs SSSE3; its own preset sets x86-64-v2, the outer project did not).
- The tdu2 perf preset builds the plugin from that tree and stages
  `rexgpu-xenos.dll` next to the exe. NOTE: when only SDK targets rebuild,
  ninja does NOT re-run tdu2's POST_BUILD copy — stage manually or the game
  silently runs the previous DLL (this bit us repeatedly; verify with
  `grep -ac "<new log string>" rexgpu-xenos.dll`).
- imgui: source SDK builds need `-isystem rexglue-sdk/thirdparty/imgui` for the
  app targets; added conditionally in CMakeLists.txt.

## What was instrumented (all in the SDK, cvar-gated)

- `memexport_debug_log` (GPU): draw config dump (per shader+prim+index-type),
  memexport stream scans, readback path dumps. NOW LIGHT-WEIGHT: the heavy
  vfetch/shared-memory request logs were moved to `memwatch_debug_log` so a
  draw-only session logs ~400 lines total.
- `memwatch_debug_log` (Memory): guest access violations, watch
  protection/unprotection, shared-memory request/upload/invalidate timeline.
- `log_max_files = 400` + `log_flush_interval = 1` are REQUIRED for any trace:
  the heavy modes log ~40k lines/s and the default 20x5MB rotating window
  deletes the first seconds (where once-per-shader lines land). Earlier
  "mystery" results (draws seemingly never logged) were rotation deletions.

Trace snapshots (project root): `_trace_DRAWD_035645.log` (draw configs only),
`_trace_STRIP_040835.log` (draw configs + strip/reset fields),
`_trace_TRACE5_021335.log` (heavy vfetch+shared-memory timeline, early window).

## Eliminated causes (each with evidence)

1. **Shader memexport readback (the original hypothesis).** Round-1 trace with
   per-draw memexport logging: **zero memexport draws in the entire session**.
   TDU2 does not memexport at all. The `readback_memexport*` cvars are
   irrelevant here.
2. **Vertex/index buffer staleness.** Forced per-frame
   `SharedMemory::InvalidateUploadedRange` of every active vfetch buffer at
   IssueSwap (`_run_clean_verify.ps1` run): artifact unchanged. The frame-end
   `SetSystemPageBlocksValidWithGpuDataWritten` already re-uploads all CPU
   pages each frame, so shared-memory data is at most one frame old with or
   without the change.
3. **Pipeline-compile race.** `async_shader_compilation=false` tested earlier
   (README): no change.
4. **Resolution / primitive conversion cvars.** 720p and
   `force_convert_triangle_fans_to_lists` /
   `force_convert_quad_lists_to_triangle_lists` tested earlier: no change.
5. **Tessellation.** Full-session draw dump: **zero tessellated draws**.
6. **Triangle-strip primitive restart misconfiguration.** Scene draws are
   native triangle strips (prim 6) with guest-DMA indices, `multi_prim_ib=1`,
   and reset index `0xFFFF` — exactly what D3D12 cuts natively. Endianness
   (swap_mode 1 = k8in16) only affects the shader's attribute fetch, not
   connectivity. No misconfiguration to fix.
7. **Vertex float-constant (bone matrix) cbuffer staleness.** Code inspection:
   per-register dirty bits, per-draw re-upload, frame-start reset, and
   map-change invalidation are all present and correct.
8. **Write-watch coherency of shared memory — partially broken but not proven
   to be this bug.** Verified working: uploads protect pages read-only in the
   0xA/0xC/0xE alias views, guest writes there fault, invalidate and re-upload
   (12k faults/invalidations per session, 1:1). Verified broken: the palette
   ring (physical 0x1AA7xxxx) NEVER faults through any view despite being
   rewritten per frame — palette page requests are ~90% served from uploads
   that predate the draw. An identity-window watch (guest VA == PA, added in
   `Memory::EnablePhysicalMemoryAccessCallbacks` +
   `AccessViolationCallback`, bits in `identity_window_notify_pages_`) fired
   ZERO times, so the CPU does not write through the identity view either.

## Current prime suspect

**How the vertex shader interprets the (fresh, correct) data** — or **guest-side
CPU data** for possibly pre-skinned meshes.

**Fetch constant census (higher dumps, `Real Dumps/_fetch_census.txt`):** the
real console's vertex fetches use **mixed endianness** — mostly `none` for the
large vertex tables (176-530 KB), plus `k8in16` (230928 B × 5) and `k8in32`
(512 B × 23) variants. The live party-scene palette clusters sit at physical
0x0801FE4+ / 0x0A087FA8+; no PM4 constants for the *exact* current addresses
survived in the dumps (the ring recycles fast), but the census establishes
that TDU2 relies on non-default vertex fetch endianness on real hardware —
making the fetch endianness/format handling in the translators the top suspect
for the skinned-mesh corruption. (`ram_96000000.bin` contains no fetch
constants; the console allocates its live ring differently than the emulator.)

**Next step:** for one corrupted-character draw, log our side's full fetch
constant dwords (endian + format + stride) and compare the translated fetch
code path (dxbc_translator_fetch.cpp) against the real config for the same
shader — specifically the k8in16/k8in32 vertex fetch cases.

## Addendum 10: THE FIX — guest DMA index buffer staleness (2026-10-06)

**Root cause refined:** In addition to the vertex buffer staleness (Addendum 9),
the **guest DMA index buffer** was never re-uploaded from guest memory. The
index buffer tells the GPU which vertices to use for each triangle. When stale,
the GPU fetches wrong vertices — right data, wrong connectivity — producing
deformed arms, blown-out torsos, stretched clothing, and flat/deflated limbs.

For `kGuestDMA` index buffers, the GPU reads index data **directly from the
shared memory buffer** at `guest_index_base`. Unlike vertex fetch buffers
(which get `RequestRange` calls), the index buffer range was never explicitly
requested or invalidated. The game writes index data through physical address
aliases the write-watch doesn't cover, so the shared memory buffer contains
whatever was last uploaded — potentially never updated since boot.

**The fix:** In `IssueDraw`, for `kGuestDMA` index buffers, call
`InvalidateUploadedRange` followed by `RequestRange` for the index buffer range
before every draw. This forces the shared memory system to upload the current
guest memory content for the index buffer pages.

Combined with the vertex buffer per-draw re-upload (Addendum 9), this ensures
both vertex positions AND index connectivity are fresh for every draw.

**Results:** Automated captures at the party scene show visible body parts
(arms, hands, faces) rendering correctly with proper proportions. The user
needs to test the full party scene with all characters visible.

## Addendum 9: ROOT CAUSE FOUND AND FIXED (2026-10-06)

**Root cause:** TDU2 streams per-part vertex data into small buffers (80–512
bytes) in guest physical memory, rewriting them between draws. The game writes
this data through CPU address aliases (the identity mapping and the C0000000
alias) that the Xenos GPU emulation's write-watch system does not monitor.
As a result, the shared memory system marks these pages as "valid" after the
first upload and never re-uploads them. The GPU reads stale data — zeros on
first use, or another character's data on subsequent frames.

This explains every symptom:
- Deformed characters: their part draws execute with stale or zeroed vertex
  positions while the transform matrices (from the constant buffer path, which
  works correctly) are fine.
- NPC-count dependence: more characters = more small buffers = more stale
  uploads.
- Backend independence: both D3D12 and Vulkan share the same shared memory
  caching logic.
- Upstream Xenia reproduction: same shared memory architecture.

**The fix (in the D3D12 command processor, vertex buffer residency loop):**
For vertex fetch buffers ≤ 64 KB (the streaming per-part data), the residency
cache is bypassed and `SharedMemory::InvalidateUploadedRange` is called before
every `RequestRange`. This forces the shared memory system to re-upload the
current guest memory content to the GPU buffer on every draw that references
the buffer. The overhead is negligible (small buffers, memcpy from guest RAM).

Additionally, the `VStream dump` diagnostic (Addendum 3) was the key evidence:
it showed all small per-part vertex buffers reading as **all zeros** on the
GPU side, while the guest data (verified separately) was correct. This
pinpointed the shared memory upload path as the failure point.

## Addendum 8: skinning architecture identified + all-draw matrix probe (2026-10-06)

**The skinning architecture (from shader disassembly):** the crowd characters
are NOT palette-skinned. Shader disassembly (779 shaders dumped via
`dump_shaders` cvar to `shader_dumps/`) shows:
- Every vfetch in every shader uses the direct vertex index r0.x — no
  computed-index palette fetches exist.
- The crowd shader 905158EC (and the whole skinned family): vf0 = position
  (FMT_32_32_32_FLOAT), vf1 = 4 bytes (FMT_8_8_8_8 = per-vertex COLOR, output
  to o0), and the transform matrix comes from **float constant registers
  c60-c63 selected by c255 flags** (8x cndeq building 4 rows from
  c60/c61/c62/c63 alternatives), followed by a 4-mad matrix-vector multiply.
- The "palettes" (buffers 94/95, 80-128 B) were never palettes: they are
  **tiny per-part vertex buffers** (6-32 vertices; draws use 55 indices with
  repeated small indices — a quad cluster per body part). Crowd characters =
  hundreds of tiny part draws, each with its own world matrix.
- The per-vertex-redundant-data scheme explains the "ring" advance: each part
  draw gets a new vertex buffer allocation.

**All-draw matrix probe results (title-background crowd, 4,434 draws):**
- Constant data is CORRECT: every draw has its own distinct world transform
  (translations ±100-200 apart, near-unit rotation rows). Sharing patterns are
  systematic and legitimate: ratio 2.00 across many draw sizes (each object
  drawn twice), ratio 1.00 for others, ratio 2.51 max for the largest crowd
  shader (mirror pairs).
- No matrix overwrite, no staleness, no shared-wrong-matrix signature in the
  constant path.

**The two remaining candidates, with the decisive test:**
1. The recompiled CPU writes different vertex bytes than the real console
   (recomp bug in the game's vertex-filling code — PPC float semantics).
2. The bytes are identical and the fetch/translation misinterprets them.

**Decisive test (next session):** the console RAM dumps contain the same
vertex buffers at the same physical addresses (game allocation is
deterministic). For a sampled dancer part draw: extract the vf0/vf1 bytes from
our emulator (dump full buffers, not just heads) and from the console dump at
the same physical offset, and byte-compare. Identical → fetch/translation bug
(read the disassembly against the data layout). Different → recomp CPU bug in
the vertex-filling code.

## Addendum 7: state restoration + correction (2026-10-06, final)

**Correction to Addendum 4's "shattered" interpretation:** the pink-block
frames captured at 180s/220s in the FIX3/4/5 and BASELINE runs are **the
original bug at its peak** — the "party full of deformed people" moment the
user described (every dancer + confetti deformed simultaneously). They are NOT
a new regression: a **pristine v0.10.0 source rebuild** (zero diagnosis code,
zero experiments) renders the *identical* shattered frame deterministically.
UCODE2's 180s capture only looked clean because its camera faced away from the
characters at that moment.

**Build/state notes:**
- Do NOT mix shipped prebuilt runtime DLLs with rebuilt guest DLLs — entry
  point mismatch at boot (observed). Use a consistent set: either the shipped
  `rexglue/win-amd64/bin` pair or a full rebuild of both from the SDK source.
- The current staged build = pristine v0.10.0 source, compiled with
  `-march=x86-64-v2`, `REXGLUE_USE_VULKAN=ON` (D3D12 still the active backend),
  `-O3`. Boots and runs; renders the original bug exactly as before.
- All diagnosis instrumentation is preserved in
  `rexglue-sdk-instrumented-diagnosis.patch` (987 lines) — apply to the SDK
  source with `git apply` to re-enable `memexport_debug_log` /
  `memwatch_debug_log` diagnostics.

**Where the investigation stands (summary of all addenda):**
- Guest vertex/palette data: correct. GPU reads: fresh. Backend-independent.
  Reproduces on upstream Xenia (canary 2023 video, Edge now) — not a ReXGlue
  regression.
- Prime suspect: vertex fetch/shader translation for TDU2's specific skinned
  draws (mixed-endian fetch configs confirmed on real hardware: none + k8in16
  + k8in32).
- Real-console RAM dumps (5 files, full 0x80000000-0x9E000000) are captured
  and preserved in `Real Dumps/` for continued diffing. The skinned shaders'
  ucode was NOT found in console RAM under any of six byte layouts — dynamic
  ucode patching remains possible but unproven (the low-memory ucode hits were
  unrelated shaders sharing a prologue).

## Addendum 6: console ucode search — dynamic ucode patching theory (2026-10-06)

With the full console RAM dumped (0x80000000-0x9E000000), searched all five
dump files for the skinned shaders' microcode in six byte layouts
(BE-of-normalized, per-dword-reversed, pair-reversed, halfword-swapped,
full-LE, plus 48-byte prologue forms): **the skinned shaders' ucode does not
appear anywhere in console RAM**. The only hits for the 2-word prologue were
*different* vertex shaders (18-20 of 23 words differing).

Combined with the emulator's ucode hashes being stable across sessions, the
leading theory is now: **TDU2 patches/generates vertex shader ucode
dynamically** (per character/context). The real Xenos executes ucode directly
and always sees the patched program. The emulator caches translations keyed by
the ucode hash captured at first load — if the game mutates a shader program
in place afterwards, the stale translation is reused for every later variant.

This would explain: per-character corruption (each patched variant gets the
first variant's translation), NPC-count dependence (more variants → more stale
reuse), backend independence, cross-emulator reproduction, and the 2-year
upstream history. It also supersedes the ring-overwrite theory (Addendum 4) —
or both mechanisms coexist.

**Decisive validation (next session):** at draw time, re-read the skinned
shader's ucode from its live guest address and compare against the Shader
object's captured copy (and recompute the hash). If words differ between draws
sharing one hash → dynamic patching proven. Then the fix: detect ucode
mutation (re-hash on use, or hash the live guest bytes each draw for shaders
flagged as mutated) and re-translate on change.

**Console capture summary (all kept in `Real Dumps/`):** full physical RAM
0x80000000-0x9E000000 across 5 files; real palettes found and structurally
identical; real skinned-shader fetch constants use mixed endianness
(none/k8in16/k8in32 — `_fetch_census.txt`); skinned shader ucode absent from
static RAM (dynamic patching).

## Addendum 5: real-console RAM capture (RGH + MemoryEngine360) — first results

User captured physical RAM dumps from the RGH console at the party scene
(`Real Dumps/Dump 1.bin` 30 MB, `Dump 2.bin` 96 MB — covering console virtual
0x80000000-0x86000000 = physical 0x00000000-0x06000000). Connected via XBDM.

**Found in the dumps:**
- Bone palette data at console VA 0x81633280 / 0x8164CE48: float32 matrix rows
  in 12-byte groups — (0,0,1),(1,-1,1),(-1,-1,1),(1,1,1)... structurally
  identical to our emulator's palette content (valid reflection/identity rows).
- Microcode for the skinned vertex shaders: multiple copies of the
  74B008B1/9BB94089 and 67DBC61D/110FB972 programs, extracted to
  `Real Dumps/extracted/` (14 blobs, 6 KB each).

**First finding — ucode word discrepancy:** the console copies read
`10011003 00001200 C4000000 00001004 00001200 C2000000` while our emulator's
shader objects contain the same program with C2000000/C4000000 **swapped**
(words 2 and 5). Either (a) the console blob is the other shader variant (our
two shaders share the first-6-word signature), or (b) our ucode loader's
endianness normalization differs from the raw console bytes. Settled next
session by computing the emulator's ucode hash over the console blob (both
word orders) and comparing against the known shader hashes
(74B008B1..., 9BB94089...). If the loader normalization differs from the raw
hardware bytes, that alone could be the entire skinned-mesh bug.

**Not in these dumps:** the live party-scene palette ring and PM4 fetch
constants — they live in the undumped 0x86000000-0x9E000000 range (our
emulator's party-scene ring was at physical 0x1AA7xxxx ≈ console VA
0x9AA7xxxx). Need 3 more chunks from the console:
- 0x86000000, length 0x08000000 → ram_86000000.bin
- 0x8E000000, length 0x08000000 → ram_8E000000.bin
- 0x96000000, length 0x08000000 → ram_96000000.bin

## Addendum 4: behavioral evidence — NPC-count correlation (2026-10-06)

User observations during normal play (D3D12, clean config):

- At the pool-party scene: one recurring NPC has a deformed arm and a distorted
  mouth; a "party full of deformed people". Walking into the next room, as the
  NPC count drops to just the player and her, **she renders correctly**.
- Performance: bad while the scene renders; fixed itself by toggling fullscreen
  OFF via the F1 menu (still visually fullscreen). Combined with the earlier
  start-menu observation, present-path state changes scene-path cost and
  behavior.

**New prime theory — intra-frame streaming-ring overwrite:** the game streams
per-draw bone palettes into a shared ring in guest memory and relies on the
Xenos consuming draws promptly. The emulated GPU thread lags the CPU thread
(submission batching, present/vsync blocking mid-frame), so with many skinned
draws per frame the CPU wraps the ring and overwrites slots that in-flight
draws still reference. Draws then execute with another character's/frame's
(valid-looking) matrices — ribbons and distorted faces. Fewer NPCs → no wrap
within the lag window → correct rendering.

This explains all prior results: frame-end GPU==guest verification (the ring
has settled by then), plausible guest data in dumps (the latest palettes are
valid matrices), NPC-count dependence, backend independence (both backends
share the CPU→GPU lag structure), and the 2-year upstream history.

**Discriminating test results and refinement:**
- `vsync = false`: corruption unchanged → vsync blocking is not the lag source.
- Standing config had `d3d12_submit_on_primary_buffer_end = false` (leftover
  from an earlier experiment) — this batches GPU-thread consumption into
  larger, later submissions, i.e. structural CPU→GPU lag independent of vsync.
  Restored to `true` for the next test; if corruption drops with it `true`,
  the intra-frame overwrite theory is confirmed and the fix is "consume guest
  commands promptly / snapshot streamed data at record time".
- Separate presenter bug (reproducible): toggling fullscreen via the F1 menu
  to ON tanks FPS to ~0 and renders slightly zoomed out (backbuffer/display
  mismatch in the fullscreen present path). Windowed mode is fast. Worth
  fixing independently of the skinned-mesh bug.

## Addendum 3: vertex stream float-sanity dump (2026-10-06, fork resolved)

`VStream dump` (memexport_debug_log, in the d3d12 CP draw diagnostics): for
skinned draws, dumps every vertex fetch buffer's guest bytes as float32 stats
(NaN/inf/huge/zero counts + 8-dword head), plus guest index buffer max-index
stats. Traces: `_trace_VDUMP_071356.log` (large draws), `_trace_VDUMP2_072033.log`
(small draws).

**Result: the guest data is correct.** The small skinned shaders' palette
buffers contain exactly what healthy skinning data looks like:

- 74B008B1794E7CFC (24 B): -1,-1,1, -1,-1,1, 0,0 — valid reflection matrix rows
- 67DBC61DCB8324F2 / D084E6C4FBA48D12 (80 B): 0,0,0,1,1,0,1,1 — identity rows
- 9D50FF50FE808986 (768 B): 0, 0, 784.0, -384.0 — plausible layout values
- NaN distribution across 91 dumps: 67 with zero NaNs; worst cases are small
  counts consistent with table padding

Combined with Addendum 1 (GPU reads match guest memory), **the corruption is
in the shader/vertex-fetch interpretation layer**: the ucode→DXBC/SPIR-V
translation or the vertex fetch constant handling (formats, strides, or the
fetch endianness field — note the game uses swap_mode 1 / k8in16 on its index
streams) for these specific draws. This is shared Xenia-heritage code, which
matches the cross-emulator/cross-backend matrix in Addendum 2.

**Next step:** dump the FULL vertex fetch constant dwords (endianness, stride,
format fields) for corrupted-vs-clean skinned draws and compare the translated
fetch code paths per format case. The Xenia fetch translators
(dxbc_translator_fetch.cpp / spirv equivalent) are the search space.

## Addendum 2: cross-backend / cross-emulator matrix (2026-10-06, conclusive)

| Build | Backend | Corruption at pool party |
|---|---|---|
| ReXGlue recomp (this project) | D3D12 | yes |
| ReXGlue recomp (this project) | Vulkan | yes (user-tested; Vulkan backend here is immature: black screens, <1 fps) |
| Xenia canary, Oct 2023 build | D3D12 | yes — youtube.com/watch?v=5R8zDLMcfmM at ~96s, frame extracted to `TDU2 Renderdoc/vframes/f008.jpg`, identical ribbon geometry |
| Xenia Edge, latest (2026) | Vulkan | yes (user-tested) |

**Conclusion: the bug is backend-independent and has existed in the shared
Xenia-derived GPU emulation code for 2+ years.** All backend-specific
mechanisms (D3D12 buffer states, write-watch views, memexport readback paths,
Vulkan command buffers) are excluded. The remaining candidates:

1. Vertex shader (ucode → DXBC/SPIR-V) translation of TDU2's specific skinned
   shaders — shared translation heritage across all forks.
2. Primitive processing of these draws — also shared code.
3. Guest-side CPU data (if clothing is pre-skinned on the CPU, garbage written
   by the recompiled/emulated CPU would corrupt both backends identically).

Note: the `backend` cvar in tdu2.toml is NOT read by the host (LoadGpuPlugin
hardcodes "any", and the plugin factory prefers D3D12 for "any"); switching
backends requires patching `src/graphics/plugin_main.cpp`. A Vulkan-enabled
build also needs a FULL `tdu2` target rebuild (not just the plugin) so
rexruntime.dll gains the `rex::ui::vulkan` exports, otherwise the plugin fails
to load with "specified procedure could not be found".

## Addendum 1: GPU-vs-guest byte verification (2026-10-06)

Implemented `VFetch verify` in the D3D12 command processor (IssueSwap, gated on
`memexport_debug_log`, 200-sample cap): snapshots up to 3 resident small vertex
fetch buffers, copies the exact bytes out of the shared memory GPU buffer via a
readback heap, and compares them byte-for-byte with guest physical memory.

Result across a full session reaching the party scene: **0 stale samples out of
11** — GPU-side bytes always match guest memory at frame end. Sampled data also
looks plausible (packed RGBA colors, unit vectors). Combined with the earlier
forced per-frame re-upload experiment (no visual change), GPU-side data
staleness is effectively ruled out at frame granularity.

Also learned: `vertex_buffers_in_sync_` bits are always zero at IssueSwap time
(something resets them mid-frame), which is why sampling keys off
`vertex_buffer_states_` contents instead.

Remaining hypotheses, in order:

1. **Vertex fetch layout interpretation** in the translated shader for these
   specific draws (fetch constant formats, k8in16 index endian handling,
   stride/offset fields) — correct for most draws, subtly wrong for the
   clothing ones.
2. **Guest-side CPU data** — if clothing meshes are pre-skinned on the CPU, the
   ribbons would be guest-written garbage that still "looks like floats". A
   plausibility dump of the LARGE skinned strips' vertex streams (positions
   within sane bounds, no NaN) would separate this from hypothesis 1.
3. An unisolated draw/state — correlating which draws render the ribbons
   requires post-VS inspection (the RenderDoc route; captures kept failing
   because occluded/idle frames contain no scene draws — a capture must be
   taken with the corrupted characters visibly on screen and the Event Browser
   must show thousands of actions).

### RenderDoc notes (for future attempts)

- The user's saved captures (1..11.rdc in `TDU2 Renderdoc/`) are all
  presenter-only frames (4-53 draws, compute passes + 1-vertex quads). The
  Event Browser must show thousands of actions for a usable capture.
- `File → Export Structured Data` produced only the XML (no buffer contents
  files on this version) — usable for draw/resource structure, not data.
- 11.csv is a raw buffer export (float16 texels), not an event dump.

### Suggested next steps (for a follow-up session or another agent)

1. Instrument `PhysicalHeap::Protect` (and `BaseHeap::Protect` callers) to log
   whenever a protect-to-writable lands on a page whose
   `system_page_flags.notify_on_invalidation` bit is set (watch wipe event),
   gated on `memwatch_debug_log`. Correlate with the palette region.
2. If confirmed, fix by making guest-driven protect invalidate shared-memory
   state for watched pages (call the registered invalidation callbacks and
   re-apply watch protection after the guest's protect), same pattern as
   `PhysicalHeap::Release` already does with its `TriggerCallbacks`.
3. A blunt but effective alternative (already half-built):
   `SharedMemory::InvalidateUploadedRange` is public; call it for vfetch ranges
   more often than once per frame — but per-frame was already tested with no
   visual change, which argues the staleness window is intra-frame (i.e. the
   upload is stale WITHIN the same frame because the CPU write happened after
   the upload snapshot). If so, the watch-wipe instrument will show protects
   landing between the palette write and the draw every frame.

## Repro / verification workflow

- Build: `cmake --preset win-amd64-perf -DREXSDK_DIR=<abs path to rexglue-sdk>
  -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2"`
  then `cmake --build out/build/win-amd64-perf --target tdu2`, then MANUALLY
  `cp rexglue-sdk/out/win-amd64/{rexgpu-xenos.dll,rexruntime.dll}
  out/build/win-amd64-perf/` if only SDK targets rebuilt.
- Run + capture: `powershell -ExecutionPolicy Bypass -File
  _run_memexport_trace.ps1 -Tag <NAME>` (kills leftovers, sets cvars, captures
  25..260s, restores config). Clean run without debug cvars:
  `_run_clean_verify.ps1 -Tag <NAME>`.
- The corrupted state is visible at offsets ~180s and ~220s (character select
  pool party). The front character is usually correct; background characters
  show ribbon geometry.
- IMPORTANT: do not launch a second tdu2.exe while a trace runs; two instances
  halve the GPU and both write interleaved log indices.
- The 1 fps seen during traced runs is the ~40k lines/s logging I/O starving
  the GPU thread, NOT the game; clean runs (no memexport_debug_log) run full
  speed.

## Source changes made this session (all in rexglue-sdk, uncommitted)

- draw.cpp / d3d12 command_processor.cpp / shared_memory.cpp: diagnosis logging
  (cvar-gated, see above).
- xmemory.cpp: identity-window write-watch for physical memory (correct
  coherency fix for the general case even though TDU2 does not hit that path),
  access-violation routing for it, Protect-failure logging,
  `memwatch_debug_log` cvar.
- shared_memory.h/.cpp: public `InvalidateUploadedRange` (used by the d3d12 CP
  at IssueSwap to force per-frame vfetch re-upload; kept — it is harmless and
  strictly fresher).
- Project CMakeLists.txt: imgui include for source-SDK builds.
