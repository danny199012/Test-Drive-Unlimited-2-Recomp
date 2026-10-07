# Findings — F1 "Fullscreen" behaviour, and the deformed-body bug

Date: 2026-10-07. This is a **read-only investigation** of the two issues reported:

1. the F1 quick-settings **Fullscreen** checkbox appears to cost FPS when on, and
   to shift the apparent FOV when toggled off (the game still fills the screen);
2. **deformed bodies** in the opening / character-select pool-party scene.

No project files were changed by this investigation. Everything below is either
a code reference into the ReXGlue SDK (github.com/rexglue/rexglue-sdk, `main`,
which is `v0.10.0` — the same source this project builds against) or a
reference into this repo.

> Method / limits: this sandbox has no Windows host, no SDK build tree and no
> game data, so nothing was executed or captured. The conclusions are from
> reading the SDK source and this repo's own diagnosis material. Where a claim
> is inferred rather than proven by a run, it is marked *(needs a run)*.

---

## Part 1 — F1 "Fullscreen": FPS impact and the apparent FOV shift

### 1.1 The checkbox is live, despite saying "Restart to apply"

The F1 menu just writes a cvar:

- `src/tdu2_menu.cpp:97-101` — the `Fullscreen` checkbox calls
  `rex::cvar::SetFlagByName("fullscreen", ...)` and sets the status text to
  `"Fullscreen updated. Restart to apply."`

That status text is **wrong**. `fullscreen` is one of the few presentation cvars
that is hot-reload:

- `src/ui/window.cpp:34-37` — the cvar is defined with the comment
  *"kHotReload (default): Window::SetFullscreen can be applied live, so the
  change callback registered in ReXApp::SetupPresentation keeps the window in
  sync whenever this cvar is changed at runtime."*
  (Contrast `window_width`/`window_height`/`video_mode_*` a few lines above and
  below, which are `.lifecycle(kRequiresRestart)`.)
- `src/ui/rex_app.cpp:350-357` — `SetupPresentation` registers
  `RegisterChangeCallback("fullscreen", ...)` which calls
  `window_->SetFullscreen(value)` **immediately**.
- `src/ui/window.cpp:275-287` → `ApplyNewFullscreen()` →
  `src/ui/window_sdl.cpp:294-299` → `SDL_SetWindowFullscreen(window, value)`.

So toggling the box in-game really does switch the OS window in and out of
fullscreen at that instant. The other two controls in that panel (render
resolution, window size) genuinely are restart-only; only the fullscreen box
is mislabelled.

Fullscreen here is SDL3 **borderless desktop fullscreen** (a NULL display mode
is SDL3's default):

- `src/ui/window_sdl.cpp:173-176`.

### 1.2 The game's FOV cannot change — what changes is the present-side scaling

The guest's idea of the display is fixed and independent of the window:

- `src/kernel/xboxkrnl/xboxkrnl_video.cpp:231-246` — `VdQueryVideoMode` builds
  `X_VIDEO_MODE` entirely from the `video_mode_width/height/refresh_rate` cvars.
- `src/kernel/xam/xam_video.cpp` — `XGetVideoMode` forwards to
  `VdQueryVideoMode`.
- Those cvars are `.lifecycle(kRequiresRestart)` (`src/ui/window.cpp:50-65`), so
  they do **not** change when the fullscreen box is toggled.

TDU2 derives its projection from that fixed video mode (and `is_widescreen`,
computed from it at `xboxkrnl_video.cpp:241`). So the game's own FOV is
identical in both states. What changes is how the presenter fits the fixed
guest image into the new surface size:

- `src/ui/presenter.cpp:854-1000` — `GetGuestOutputPaintFlow` computes
  `output_width/height` from the surface size and the guest
  `display_aspect_ratio`, then applies the overscan "safe area"
  (`present_safe_area_x/y`, default 90) and `present_letterbox`. If the surface
  aspect differs from the guest aspect it **crops (zooms) before it
  letterboxes**.
- `src/ui/d3d12_presenter.cpp:604-608` — the host render target it paints into
  is `paint_context_.swap_chain_width/height`, i.e. the window/surface size.
- `src/ui/d3d12_presenter.cpp:341-467` — on a size change the swap chain is
  `ResizeBuffers`-ed, its buffers re-fetched and RTVs re-created, and the DXGI
  scaling mode is chosen (`DXGI_SCALING_NONE` only when the swap chain exactly
  equals the surface, otherwise `DXGI_SCALING_STRETCH`).

So: windowed = 1600x900 surface (16:9); fullscreen = monitor surface. If the
monitor/desktop is 16:9 **and** the safe-area settings are left alone, the two
should look identical. A visible "FOV" difference therefore tells us the
surface aspect is changing — e.g. a 16:10 / 21:9 desktop, a non-16:9
`window_width/height`, or the safe-area crop path kicking in.


### 1.3 Why it can still look "fullscreen" after unchecking

`SDL_SetWindowFullscreen(window, false)` restores the window to the rectangle it
had before going fullscreen — the size the window was **created** at.
`window_width`/`window_height` are restart-only and are what SDL restores to.
If that size is at/near the desktop size, the windowed state still covers the
screen, and because the presenter always scales the guest image to the whole
window, it looks fullscreen. Worth confirming the actual window rect with the
F3 debug overlay while toggling *(needs a run)*.

### 1.4 Where the FPS goes

Two separate costs, worth separating before treating this as one bug:

1. **Fullscreen itself.** Entering borderless fullscreen changes the surface to
   the monitor resolution, so the presenter's paint target jumps from the
   window size to the monitor size (`d3d12_presenter.cpp:604-608`), the swap
   chain is resized/re-created (`:341-467`), and presentation runs borderless
   with `DXGI_PRESENT_ALLOW_TEARING` (`:411-414`, `:1150-1160`). Painting the
   guest image into a 4K target instead of a 1600x900 one is a real,
   roughly quadratic cost in the present path. Launching with
   `fullscreen = true` so the swap chain is *created* at the right size avoids
   the live resize, but not the larger target.
2. **Having the F1 panel open at all.** While any ImGui drawer is open the
   presenter cannot use the low-latency guest-output-thread present path and
   falls back to painting on the UI thread:
   - `src/ui/presenter.cpp:1300-1330` — `UpdatePaintModeToDesired` returns
     `kUIThreadOnRequest` when `!ui_drawers_.empty()`, and
     `kGuestOutputThreadImmediately` only when nothing is drawn on top.
   So part of the "F1 menu costs FPS" impression is the menu, not the
   fullscreen flag. Toggling the flag and then closing the menu is the fair
   test.

### 1.5 Recommendations (nothing changed)

- Fix the label/status in `src/tdu2_menu.cpp:100` — fullscreen is live, not
  restart-only. Cosmetic, but actively misleading.
- For identical framing between windowed and fullscreen, make the surface
  aspect match the guest aspect: set `window_width/height` to the monitor's
  native resolution, or neutralise the crop/letterbox knobs
  (`present_allow_overscan_cutoff = false`, or `present_safe_area_x/y = 100`,
  `present_letterbox = false`). Any of these removes the crop that reads as an
  FOV change.
- For the FPS hit, prefer launching with `fullscreen = true` (create the swap
  chain at the final size once) and judge cost with the F1 panel closed, so the
  UI-thread present fallback is not counted.


---

## Part 2 — Deformed bodies at the character-select / pool-party scene

### 2.1 What this repo already established (from `DIAGNOSIS_TDU2_SKINNED_MESH.md`)

The project has done a lot of work here. The load-bearing facts:

- **Guest data is correct.** The tiny per-part vertex buffers contain valid
  values (`Addendum 3`), and at frame end the GPU-side bytes match guest memory
  (`Addendum 1`). So it is not "the game wrote garbage".
- **It is backend-independent and reproduces in stock Xenia** (Canary D3D12,
  Edge Vulkan) — `Addendum 2`. So it is not a ReXGlue-recomp regression, and
  not D3D12-specific.
- **The crowd characters are not palette-skinned.** Each body part is a tiny
  vertex buffer (80-512 B) plus a per-part world matrix in float constants
  (`Addendum 8`). Corruption is therefore either wrong vertex *data* or wrong
  *connectivity* (indices) for those tiny per-part draws.
- **Everything frame-granular was ruled out:** per-frame forced re-upload, the
  memexport knobs, `async_shader_compilation=false`, resolution, primitive
  conversion, tessellation, strip-restart (`Addendum 1`, README 2026-10-05).

The doc's own `Addendum 10` ("THE FIX") concludes the remaining hole is
**per-draw staleness of the streamed buffers inside a frame**, and proposes
invalidating the small vertex buffers and the guest DMA index buffer before
each draw.

### 2.2 What I verified in the SDK source

I re-read the relevant SDK code (`main` == `v0.10.0`, the version this project
builds) to check that theory against what the code actually does.

**The shared-memory mirror is only refreshed when a range is requested.**
`SharedMemory` keeps a GPU-side mirror of guest physical memory and tracks a
per-page "valid" bit. Pages become valid when they are uploaded, and at frame
end:

- `src/graphics/shared_memory.cpp:104-107`
  `SetSystemPageBlocksValidWithGpuDataWritten()` sets
  `system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_` — i.e.
  **pages that were valid only because the CPU uploaded them lose their valid
  bit at the end of every frame**, so the next frame re-reads them from guest
  memory.
- `SetSystemPageBlocksValidWithGpuDataWritten()` is called from
  `src/graphics/d3d12/command_processor.cpp:3456` (in `IssueSwap`).

So across frames the mirror is re-read. The dangerous window is **within a
single frame**, and the only thing that can invalidate a page mid-frame is the
physical-memory write watch
(`SharedMemory::MemoryInvalidationCallback`, `shared_memory.cpp:478-537`),
which depends on the guest write actually faulting.

### 2.3 The concrete gap: the guest DMA index buffer is never requested

This is the finding I would act on first, because it is a plain omission in the
code and it exactly matches the symptom (right vertices, wrong connectivity →
stretched "ribbon" triangles reaching toward other characters' positions).

For a `kGuestDMA` index buffer the GPU reads the indices **directly out of the
shared-memory mirror**:

- `src/graphics/d3d12/command_processor.cpp:2645-2665` —
  `index_buffer_view.BufferLocation = shared_memory_->GetGPUAddress() +
  primitive_processing_result.guest_index_base;` (and, when memexport is used,
  a `D3DCopyBufferRegion` from the same mirror).

But nowhere in the D3D12 command processor is `RequestRange` called for the
index buffer range. The only `RequestRange` calls are for **vertex fetch
buffers** (`:2514`), **memexport ranges** (`:2538`) and a full-memory fallback
(`:2548`). The Vulkan command processor has the same structure.

Consequence: the mirror pages holding the index data are uploaded only if some
*other* request happens to cover them (a vertex fetch in the same allocation, a
texture upload, the full-memory fallback). The indices the GPU actually reads
are therefore whatever was last mirrored into those pages — potentially from an
earlier frame or a different character. Wrong indices → the GPU assembles
triangles from unrelated vertices → exactly the "ribbons"/stretched wedges.

**This gap is upstream, in both Xenia branches** — which explains the two-year
history:

- `xenia-project/xenia` `src/xenia/gpu/d3d12/d3d12_command_processor.cc:2354-2378`
  — same `GetGPUAddress() + guest_index_base`, `RequestRange` only for vfetch
  (`:2231`) and memexport (`:2253`).
- `xenia-canary/xenia-canary` `src/xenia/gpu/d3d12/d3d12_command_processor.cc`
  — same at `:3159-3183`, `RequestRange` only at `:3013` and `:3035`.

That is why the artifact is backend-independent, cross-emulator, and never
logged: nothing is wrong with the data, it is just not the current data.


### 2.4 Second gap: the intra-frame vertex-buffer residency fast path

The vertex fetch loop skips `RequestRange` when the fetch constant is unchanged
since the previous draw in the same frame:

- `src/graphics/d3d12/command_processor.cpp:2509-2512` —
  `if (state.address == vfetch_constant.address && state.size ==
  vfetch_constant.size) { vertex_buffers_in_sync_[...] |= vfetch_bit; continue; }`
- `vertex_buffers_in_sync_` is reset only at frame end
  (`:1897-1898`, in `IssueSwap`) and on cache clears (`:112-125`).

TDU2 streams per-part vertex data into small buffers and **reuses the same
address/size** for different parts. Within one frame, the second and later
draws that reuse an address therefore never call `RequestRange`; they rely
entirely on the write watch having invalidated the page. If the watch does not
fire for the address alias the game writes through — which is what the
project's own instrumentation reported for the palette ring
(`DIAGNOSIS_TDU2_SKINNED_MESH.md`, "Current prime suspect" / eliminated cause 8)
— those draws read the previous part's vertices.

This is the same class of bug as 2.3 and is the "small vertex buffers" half of
`Addendum 10`.

### 2.5 Note: the repo's patch does **not** contain the doc's "THE FIX"

Worth flagging because it is easy to assume otherwise:
`rexglue-sdk-instrumented-diagnosis.patch` is the *diagnosis* patch. It adds
`memexport_debug_log`/`memwatch_debug_log` logging, the `VStream dump` /
`VFetch verify` probes, and it adds `SharedMemory::InvalidateUploadedRange`, but
its only residency change is an **experiment gated on `memwatch_debug_log`**
that re-uploads *all* vertex buffers each frame and, per the file's own comment,
"showed no visual effect". There is **no per-draw index-buffer request and no
small-buffer invalidation** in the patch. So the fix described in `Addendum 10`
is not actually present in this tree — it was either never committed or lived
only in a session's working copy.

### 2.6 Proposed fix (ready to apply to the SDK source)

Minimal, targeted change in
`src/graphics/d3d12/command_processor.cpp`. In `IssueDraw`, after the existing
`RequestRange` block for vertex fetch / memexport (around line 2556, before the
topology switch and before the index buffer is bound), add:

```cpp
// The guest DMA index buffer is read by the GPU straight out of the shared
// memory mirror, but unlike the vertex fetch and memexport ranges it was
// never requested, so the mirror can hold indices from an earlier frame or a
// different draw. Request it every draw (the ranges are tiny - a few hundred
// bytes) so connectivity is always current.
if (primitive_processing_result.index_buffer_type ==
    PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
  uint32_t index_size_log2 =
      primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 1 : 2;
  uint32_t index_range_bytes =
      primitive_processing_result.host_draw_vertex_count << index_size_log2;
  if (!shared_memory_->RequestRange(primitive_processing_result.guest_index_base,
                                    index_range_bytes)) {
    REXGPU_ERROR(
        "Failed to request index buffer at 0x{:08X} (size {}) in the shared memory",
        primitive_processing_result.guest_index_base, index_range_bytes);
    return false;
  }
}
```

If the index pages are being served stale from *within* the frame (i.e. the
page was already marked valid earlier in the frame), prefix the
`RequestRange` with `shared_memory_->InvalidateUploadedRange(base, bytes);`
(that method exists in the project's patch and is a natural SDK addition).
Applying the same treatment to **vertex fetch buffers below ~64 KB** covers the
2.4 half — that is the "small buffers" threshold `Addendum 9` proposed.

For a quick A/B, gate both behind a new cvar (e.g.
`tdu2_force_guest_index_request`, `tdu2_force_small_vfetch_reupload`) so the
"party scene can be compared with the behaviour on and off in one build.

### 2.7 The decisive test (do this before/with the fix)

The project's own patch already contains almost the exact probe needed. The
`VStream dump` diagnostic logs, for guest DMA index buffers:

```
VStream dump: VS ucode <hash> index buffer 0x<base> count <n> max index <m>
```

(patch lines ~532-548, gated on `memexport_debug_log`). Run the party scene
with that enabled and compare `max index` against the vertex count the vfetch
buffers actually provide for the same draw:

- **`max index` >= vertex count** → the index data is stale/wrong → this
  confirms 2.3 and the fix above is the answer.
- **`max index` is in range and stable, and the vertices are correct** → the
  data and connectivity are both fine, and the remaining suspect is the
  shader/fetch translation (see 2.8).

A cheaper version of the same test that does not need the heavy trace: log the
guest index buffer bytes twice in one frame for a corrupted draw (once at the
first `RequestRange`-less use, once at `IssueSwap`) and diff them. If they
differ, the mirror is stale within the frame.

### 2.8 If 2.3/2.4 are not it: the remaining candidates, in order

1. **Vertex fetch translation for the exact formats TDU2 uses.** The console
   fetch census shows mixed endianness (`none` + `k8in16` + `k8in32`) for the
   skinned draws, while the large tables are `none`. The endianness/format
   handling lives in
   `src/graphics/pipeline/shader/dxbc_translator_fetch.cpp:214-263` (and the
   SPIR-V equivalent). This is shared heritage with Xenia, so a bug here also
   reproduces upstream. The concrete test is to dump the full fetch-constant
   dwords for one corrupted draw and compare the translated path against the
   format the constant declares.
2. **Dynamic ucode patching** (`Addendum 6`): if TDU2 rewrites a shader's
   microcode in place, a translation cached by ucode hash would be reused for
   every later variant. Unproven, and the console dumps did not contain the
   skinned ucode, so this needs the "re-hash the live guest ucode at draw time"
   check the doc describes.
3. **Primitive processing of these strips** — largely ruled out by the
   `force_convert_*` tests, but the guest-DMA index path is the one part of
   primitive processing that has never had a residency check, which is why 2.3
   is the better first bet.

### 2.9 One-line summary

- **Fullscreen/FPS/FOV**: the F1 checkbox is hot-reload, not restart-only; the
  FOV difference is the presenter re-fitting a fixed 16:9 guest image into a
  different surface aspect (safe-area crop / letterbox), not a guest FOV change;
  the FPS cost is the swap-chain resize plus a monitor-sized present target,
  with an extra cost from simply having the ImGui panel open.
- **Deformed bodies**: the strongest, code-verified candidate is that the
  **guest DMA index buffer is never requested into the shared-memory mirror**,
  in ReXGlue *and* in both Xenia branches — right vertices, stale connectivity,
  no log, backend-independent, two-year history. Requesting the index range per
  draw (and, secondarily, re-requesting small vertex buffers) is a small,
  testable fix; the existing `VStream dump` "max index" probe is the decisive
  check.

