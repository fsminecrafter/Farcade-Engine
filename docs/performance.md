# Editor performance on Minimal-OS

Measure with `farcade-editor.run --bench N [--farcadegl] [--profile]` (phases
idle, mouse, orbit, settle; see `editor/bench.h`) and, on the host,
`build/host/bin/fe-editor-bench [--farcadegl] [--adaptive]`.

What the editor does:

- **Redraw only what changed.** The UI is built every frame (about 5 ms under
  QEMU TCG), but the ImGui draw lists are hashed; unchanged frames are not
  rasterized or presented, changed ones redraw only the rectangles of the changed
  lists (before and after) and present only those (`SwRenderer::render_incremental`,
  `platform::present_region`). The result equals a full redraw
  (`SwRenderer::set_verify`, on in `editor_tests`). An idle editor waits 40 ms
  between frames.
- **Adaptive viewport resolution** (`Editor::adaptive_view`, on in main_os,
  `--no-adaptive` turns it off): while the view changes, the image is rendered at
  1/d size (d from the measured ms per pixel, budget 200 ms) and stretched; with
  FarcadeGL it uses cheap draft shaders. When the view stops, the CPU preview is
  redrawn sharp; FarcadeGL redraws the full image in bands of about 250 ms, one
  per UI frame (`Renderer::render_rows`), so the UI stays usable and a new drag
  drops the rest.
- FarcadeGL on QEMU TCG is dominated by fragment shading in the kernel's FGPU
  (about 6 us per pixel per CPU, linear in pixels, scales with -smp); shader
  compile, upload and readback are under 30 ms. On KVM/hardware d is 1.

Not done: multi-core UI rasterization. After the damage tracking the UI raster
is 1-5 ms typical and about 20 ms for a viewport repaint, so threads (and their
risk) were not worth it.
