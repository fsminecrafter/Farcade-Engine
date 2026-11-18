# Farcade Engine icons

Vector icon set for the editor (102 icons). Each icon is one SVG in this folder; `manifest.json` groups them and gives
each a one-line meaning (the preview and the checker read it).

## Spec

- File: `<name>.svg`, snake_case of the `Icon` enum value in `editor/src/icons.h` (`EyeOff` -> `eye_off.svg`).
- `viewBox="0 0 24 24"`, artwork inside a 2 px safe margin (20x20 live area). Keylines: circle d20, square 18x18,
  portrait 16x20, landscape 20x16. Coordinates on whole or half pixels so 24 and 48 px render crisply.
- Line style: root element carries `fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round"
  stroke-linejoin="round"`. Children inherit; no child overrides the stroke width.
- At most one soft fill layer per icon: an element with `fill="currentColor" fill-opacity="0.18"` (two-tone).
- Colors: only `currentColor` and `none`. The editor tints icons with the theme.
- Exceptions: `play` is solid `currentColor`; `info`, `warning`, `error` are filled glyph badges
  (`fill="currentColor" stroke="none" fill-rule="evenodd"`) with the inner mark knocked out.
- Allowed elements: `svg g path rect circle ellipse line polyline polygon`. `g` only with `transform`
  (rotate/translate/scale). No text, filters, masks, clip paths, images, classes, `<style>`, or metadata.
- Legibility: tested at 14 px; no detail finer than about 1.5 px. 3D objects share one isometric-ish view
  (hexagon cube: top face soft-filled, 30 degree edges).
- Note: `pause`/`stop` use the soft fill (only `play` is solid), `more_horizontal`/`drag_handle` are dots drawn as
  small soft-filled rings, and `palette`/`keyboard`/`help` dots are zero-length round-capped segments.

## Check and preview

    python3 tools/icons/check.py     # validates every SVG, the enum coverage and manifest.json
    node tools/icons/preview.mjs     # needs playwright + Chromium; writes preview-{dark,light}[@2x].png here

The sheets show each icon at 14, 16, 20, 24 and 48 px on dark (#191B20, icons #DCDFE4) and light (#F4F5F7, icons
#1C1F24) backgrounds; `pointer`, `move`, `eye`, `folder` are drawn in the accent to judge selected tinting.

## Icons

### Tools and gizmos

| name | meaning |
|---|---|
| `pointer` | Select tool / arrow cursor |
| `move` | Move gizmo (four-way arrows) |
| `rotate` | Rotate gizmo (ring with arrow and pivot) |
| `scale` | Scale gizmo (box with diagonal arrow) |
| `world` | World space (globe) |
| `local` | Local space (tilted box with its own axes) |
| `magnet` | Magnet |
| `grid` | Grid |
| `snap` | Snapping (crosshair target) |
| `play` | Play the scene (solid) |
| `pause` | Pause |
| `stop` | Stop |

### Files

| name | meaning |
|---|---|
| `folder` | Folder |
| `folder_open` | Open folder / open project |
| `file` | Generic file |
| `script` | Script file |
| `image` | Image file |
| `model` | 3D model file (faceted mesh) |
| `audio_file` | Audio file |
| `scene_file` | Scene file |
| `shader_file` | Shader file |
| `save` | Save (floppy) |
| `export` | Export (arrow out of tray) |
| `import` | Import (arrow into tray) |
| `package` | Package / build bundle |

### 3D objects

| name | meaning |
|---|---|
| `cube` | Cube mesh |
| `sphere` | Sphere mesh |
| `cylinder` | Cylinder mesh |
| `plane` | Plane mesh (thin slab) |
| `collider` | Collider (wireframe box) |
| `body` | Rigid body (ball on ground) |
| `physics` | Physics (atom orbits) |
| `joint` | Joint (linked nodes) |
| `empty` | Empty object (pivot with dashed ring) |
| `sprite` | Billboard / sprite (ghost) |
| `text_t` | 3D text |
| `material` | Material (glossy ball) |

### Scene objects and modules

| name | meaning |
|---|---|
| `camera` | Camera |
| `bulb` | Light module |
| `light_point` | Point light |
| `light_spot` | Spot light |
| `light_directional` | Directional light |
| `sun` | Sky light / sun / quality |
| `cloud` | Sky |
| `wand` | Post process / effects |
| `sparks` | Particle effects / sparkles |
| `particles` | Particle emitter |
| `wind` | Force field |
| `spawn` | Spawner (pad with arrow) |
| `speaker` | Audio sampler / speaker |
| `wave` | Audio stream / waveform |
| `ear` | Audio listener |
| `reverb` | Reverb zone |
| `clip` | Animator clip (film strip) |
| `animation` | Animation (keyframes) |
| `layers` | Layers / scene (stacked sheets) |
| `monitor` | Device / display |
| `orthographic` | Orthographic projection |
| `perspective` | Perspective projection |
| `star` | Favorite / main object |

### Panels

| name | meaning |
|---|---|
| `hierarchy` | Hierarchy panel |
| `inspector` | Inspector panel |
| `files` | Files panel |
| `console` | Console panel |
| `settings_sliders` | Settings / properties (sliders) |
| `palette` | Theme (palette) |
| `keyboard` | Keyboard shortcuts |
| `gear` | Settings (cog) |

### Actions

| name | meaning |
|---|---|
| `plus` | Add |
| `minus` | Remove / collapse |
| `close` | Close / clear |
| `check` | Confirm / checked |
| `search` | Search |
| `filter` | Filter (funnel) |
| `sort` | Sort (up/down arrows) |
| `refresh` | Refresh / reload |
| `up` | Up one level / move up |
| `trash` | Delete |
| `undo` | Undo |
| `redo` | Redo |
| `copy` | Copy |
| `paste` | Paste |
| `duplicate` | Duplicate |
| `lock` | Locked |
| `unlock` | Unlocked |
| `link` | Linked |
| `unlink` | Unlinked |
| `eye` | Visible |
| `eye_off` | Hidden |
| `chevron_down` | Expanded disclosure |
| `chevron_right` | Collapsed disclosure |
| `more_horizontal` | More actions (three dots) |
| `drag_handle` | Drag handle (grip dots) |
| `maximize` | Maximize |
| `restore` | Restore down |
| `fullscreen` | Enter fullscreen |
| `fullscreen_exit` | Exit fullscreen |
| `bug` | Debug / bug |
| `help` | Help |

### Status badges (filled glyphs)

| name | meaning |
|---|---|
| `info` | Information badge |
| `warning` | Warning badge |
| `error` | Error badge |

## How the editor uses them

The SVGs are the source. The editor embeds them as Minimal-OS vector images
(the `.img` format's vector mode, version 2; Minimal-OS `docs/media.md`,
SDK `docs/images.md`) and draws them through its fonts:

1. `tools/icons/build_pack.py convert <path to Minimal-OS>/build/tools/mosmedia`
   converts every SVG (and `docs/logo/farcade-logo.svg` as `logo`) to
   `editor/icons/img/<name>.img` (build `mosmedia` with
   `make build/tools/mosmedia` in Minimal-OS). The `.img` files are
   committed, so building the editor needs no Minimal-OS tools.
2. `tools/icons/build_pack.py header` gives new icons a codepoint in the
   Private Use Area (`codepoints.txt`, append-only) and rewrites
   `editor/src/icon_glyphs.h` (`FE_ICON_<NAME>` strings).
3. The build embeds the `.img` files (`mk/editor.mk`), and
   `editor/src/icon_font.cpp` rasterizes each one with the SDK's
   `mimg_render_ex` into the font atlas at the size a frame asks for,
   tinted through the "current color" paint.

So an icon can sit in any label (`ImGui::Button(FE_ICON_SAVE "  Save")`),
and `draw_icon(dl, Icon::..., pos, size, color)` draws one in a box. To add
an icon: draw `editor/icons/<name>.svg` to the spec above, run
`python3 tools/icons/check.py`, then steps 1 and 2. To use it through the
`Icon` enum as well, add the enum value (icons.h) and its file name in
`kIconFile` (icons.cpp).
