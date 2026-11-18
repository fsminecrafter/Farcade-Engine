# Graphics: the GPU tier of the FarcadeGL renderer

## Texture and custom shader assets

A mesh material can reference an image texture or a combined FGSL shader file.
Use the Inspector's Texture and Shader slots, or assign paths from a script
through `object.material.texture` and `object.material.shader`. Script inputs
declared as `file` (or `asset`) are path strings and can be dropped on from the
Files panel.

The `.fshader` format uses the existing FGSL syntax and two comment markers:

```glsl
// @vertex
uniform mat4 fgl_ModelViewProjection;
attribute vec3 position;
attribute vec2 texcoord;
varying vec2 uv;
void main() {
    uv = texcoord;
    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);
}

// @fragment
uniform sampler2D u_texture;
varying vec2 uv;
void main() {
    gl_FragColor = texture2D(u_texture, uv);
}
```

`u_texture` is bound to the material image; without one it is a white texture.
Built-in meshes currently get generated planar UVs, and texture alpha is not
blended by the XRGB material path.
The renderer also supplies the usual FarcadeGL matrix uniforms and `u_mat`
(material color and roughness). A custom shader replaces the built-in material
shading. When a visible mesh has a texture or custom shader, the renderer uses
its base FarcadeGL path for that frame, so advanced GPU-tier effects (such as
shadows, SSAO and fog) are not applied.

`fe::gfx::Renderer` (engine/gfx) has two tiers, set by `GraphicsSettings`
(engine/gfx/graphics_settings.h).

- **Base tier**: forward lighting (Blinn-Phong), sky, tonemap and grade folded
  into the shaders. Runs everywhere, also on FarcadeGL's CPU backend.
- **GPU tier**: shadow maps, SSAO, bloom, FXAA and PBR (GGX) shading. It runs
  when FarcadeGL runs on a graphics card (`FGPU_CAP_HW_3D`), or when
  `allow_gpu_effects_on_cpu` forces it (slow; tests, screenshots).
- **Fog** (`Sky.fog`) works in both tiers.

## Feature levels

`engine/gfx/feature_level.h` is the one place that turns what the device says
(`fglGetDeviceInfo` caps) into the pipeline that runs. Settings > Graphics >
Device shows the level and lets you request one (`GraphicsSettings::feature_level`);
`Renderer::features().level`, `Renderer::feature_level()` and `Renderer::caps()` give it
to code.

| Level | When | What runs |
|---|---|---|
| FL0 | today's FGPU on the CPU backend | the base tier: forward lights, ambient, sky, post folded into the shaders, CPU GI |
| FL1-ready | FL0 plus scissor, mipmaps and 16 texture units | the same, with the passes that need those caps taking their cheaper path (below) |
| GPU tier | a graphics card (`FGPU_CAP_HW_3D`), or forced | the multi pass pipeline of the next sections |

`Auto` picks the highest level the caps allow. Asking for more than the device has is
clamped (FL1-ready without the caps gives FL0) except the GPU tier, which can always be
forced on the CPU backend (slow; the old `allow_gpu_effects_on_cpu` switch does the same).
The optional hardware features are cap bits (`PassCaps`: `scissor`, `mipmaps`, `mrt`,
`tex16`); `DeviceCaps::from_abi` reads them from `FGPU_CAP_SCISSOR`, `FGPU_CAP_MIPMAPS`
and `FGPU_CAP_MRT` as soon as the kernel's `fgpu_abi.h` defines them, and the texture unit
count from `FGPU_MAX_TEXTURE_UNITS`; until then they are off. Nothing branches on the backend name.

## Post processing stack

Every final-colour shader (base tier and GPU tier) gets the same generated chain
(`shaders_post.h`): exposure and tint, tonemap, contrast, saturation, lift / gamma / gain,
vignette, film grain, sRGB. Each choice (operator, grading, grain) is compiled into the
shader, not branched on (the host shader machine runs `if` as predication), so the default
look is not slower than before and a neutral grade costs nothing. Changing the operator
recompiles the shader once.

| Setting (Post Processing object) | Notes |
|---|---|
| Tonemap | None, Reinhard, ACES, AgX (Sobotka's look: Rec.2020 inset matrix, log2 over -12.47..4.03 EV, a polynomial sigmoid) |
| Lift / Gamma / Gain | `out = pow(max(c * gain + lift * (1 - c), 0), 1 / gamma)` after tonemap and saturation |
| Film grain | 0..1, interleaved gradient noise, a new pattern every frame, weaker in the highlights |
| Chromatic aberration | 0..1, GPU tier only (3 taps in the composite pass, linear filtering) |
| Bloom radius | 0..1: the weight of the wide levels in the upsample chain (0.5 = the old look) |
| Auto exposure | see below; Exposure becomes a compensation |

**Bloom** is a chain of up to 5 levels (1/4 to 1/64; a level needs 4 texels on each side, so a small
image gets fewer): a bright pass with a soft knee and Karis averaging (no fireflies), a 13 tap
downsample at each level, and a tent upsample that adds the wider level scaled by `2 * radius`.

**Auto exposure** costs no GPU pass. After each frame the renderer meters the image it already
read back: 64 x 36 samples with a mild centre weight, each undone through the sRGB curve, the
tonemap (inverted from a table) and the exposure that was applied, so the answer is the scene's
average luminance (geometric mean) whatever the exposure was. The controller eases the automatic EV
towards the value that maps that mean to 0.18, with the rate `exposure_speed` (1 / s) and the range
`auto_exposure_min / max`; `RenderScene::delta_time` is the step (negative: wall clock, at most
0.25 s). It works on FL0 and the GPU tier alike. The measurement is a frame late (the first
frame is drawn with 0 EV), and contrast / saturation / grading are not undone, so heavy grading
shifts the target a little. `RendererStats::exposure_ev` and `meter_log2` show what happened.

**Internal resolution.** `render_scale` (0.25..1) renders every pass at that fraction (sizes rounded
to multiples of 8) and upscales bilinearly when the image is read back; the stats report
`internal_width / internal_height`. A 0.5 scale costs about a quarter of the shading. **Dynamic
resolution** (`dynamic_resolution`, `target_frame_ms`, `min_render_scale`) moves the scale between
`min_render_scale` and `render_scale` to hold the target: the step follows the square root of the
time ratio, down by up to 15% a frame, up by at most 3% and only with 15% of the time to spare.
It uses the measured frame time (render, readback and upscale). Bands (`render_rows`) always draw at
full size. Screenshots: `gfx-agx.png`, `gfx-graded.png`, `gfx-autoexposure.png`, `gfx-post-high.png`
(`fe-gfx-shot --tonemap agx --grade warm --auto-exposure --scale 0.5 ...`).

## Graphics settings in games

The graphics settings are part of the game, not only of the editor. `fe::gfx::GraphicsSettings`
(engine/gfx/graphics_settings.h) is one plain struct, and `graphics_settings.cpp` stores it as text, by
field name, in the style of `.fscene`:

```
farcade-graphics 1
preset high
aa fxaa
shadows cascades
shadow_cascades 3
render_scale 0.75
...
```

```cpp
char text[2048];
size_t n = graphics_settings_to_text(settings, text, sizeof text);      // save: put `text` into project.fe
GraphicsSettings g;
int ignored = graphics_settings_from_text(&g, text, strlen(text));      // load at startup, then
renderer.set_settings(g);                                               // apply (cheap; every frame is fine)
```

`from_text` starts from the defaults, so a missing name keeps its default; unknown names and invalid
values are ignored (the return value counts them); a `preset` line names the preset when nothing differs
from it, else the settings read back as `custom`. A null text returns -1. Every option is an explicit choice
and the older, cheaper technique stays selectable next to the newer one; Low is the cheap base tier of
before. Names and allowed values:

| Name | Values |
|---|---|
| `preset` | low, medium, high, ultra, custom (setting it applies the preset) |
| `feature_level` | auto, fl0, fl1, gpu |
| `render_scale` | 0.25 to 1; `dynamic_resolution` bool; `target_frame_ms` 8 to 100; `min_render_scale` 0.25 to 1 |
| `shadows` | off, single (one map), cascades; `shadow_cascades` 2 or 3 (with cascades) |
| `shadow_resolution` | 512, 1024, 2048, 4096; `shadow_distance` 1 to 2000 m; `soft_shadows`, `cascade_cache` bool |
| `local_shadows` | bool; `local_shadow_lights` 1 to 4; `local_shadow_resolution` 512 to 4096 |
| `ao` | off, ssao, gtao; `ao_radius` 0.05 to 5; `ao_strength` 0 to 2; `ssr` off, on |
| `aa` | off, fxaa, smaa, taa |
| `bloom` | off, simple (4 levels, the first version), chain (5 levels, radius) |
| `pbr` | bool (GGX, else Blinn-Phong) |
| `allow_gpu_effects_on_cpu` | bool (run the GPU tier on the CPU backend, slow) |

Look and style settings (tonemap operator, exposure, grading, bloom strength, grain, fog) belong to the scene's
Post Processing and Sky objects, not to this struct. Code can also use the property API
(`graphics_property_count / graphics_property`, `graphics_get_text`, `graphics_set_text`,
`graphics_apply_preset_name`), which is what Settings > Graphics and the Lua binding use.

**Scripts** read and change them at run time with `fe.graphics` (engine/script/lua_graphics.h):

```lua
fe.graphics.get("aa")                  -- "fxaa" (strings for choices, numbers, booleans; shadow_resolution is a number)
fe.graphics.set("aa", "off")           -- string, number or boolean; an unknown name or invalid value is a Lua error
fe.graphics.set("render_scale", 0.6)
fe.graphics.preset("low")              -- low | medium | high | ultra
for _, p in ipairs(fe.graphics.list()) do print(p.name, p.type, p.value, p.values and table.concat(p.values, "/")) end
fe.graphics.text()                     -- the settings as text
```

A change is written to the host's `GraphicsSettings` and sets the host's `changed` flag; the host calls
`Renderer::set_settings` before the next frame. The host connects them with
`ScriptRuntime::set_graphics_settings(&settings, &changed)` (the editor does it for Play and restores the settings at
Stop); without it `fe.graphics` raises "the graphics settings are not available here".

## Probe GI (FL0 hybrid GI, CPU side)

`gi` = `probes` (Settings > Graphics > Global illumination; `hemisphere` is the old ambient, free, and the
default of Low and Medium) replaces the diffuse hemisphere ambient with light the scene itself bounced.
It is CPU work, not a pass, so it runs on every feature level (`engine/gfx/gi.h`):

1. **Voxels.** A 48^3 grid of 0.5 m voxels around the camera is built from the draw items by conservative
   triangle / box voxelization (every voxel a triangle touches; albedo, normal, emissive = unlit items).
   It is rebuilt when the scene changed (at most every 100 ms, so moving objects re-splat at 10 Hz) or when the
   camera leaves the middle third of the grid (no toroidal scrolling yet).
2. **Light injection.** A quarter of the surface voxels a frame get the sun (with a voxel DDA shadow ray of at
   most 64 steps), up to 8 local lights (no shadow), emissive, and the irradiance the probes had last frame
   times `SkyLight.gi_bounce`: multi-bounce, one frame late.
3. **Probes.** A window of 16 x 8 x 16 L1 SH probes at 2 / 1 / 0.5 m (`gi_density`) around the camera, toroidal
   (a probe keeps its world cell while the window slides; new cells start from the hemisphere). 1/32, 1/16 or
   1/8 of them (`gi_update`) shoot 32 rays through the voxels each frame, never-seen probes first, blended with
   hysteresis 0.93. A probe that moved out of geometry (it tries its 6 neighbours) and one whose rays mostly hit
   back faces is marked invalid and keeps the hemisphere.
4. **Shaders.** The probes become a 64 x 128 ARGB8 texture (4 coefficient tiles; the DC term as sqrt(v / 4), the
   directional terms signed), sampled with 8 linear taps (hardware filtering inside a tile, a manual lerp
   between two slices) in the base and the GPU tier's lit shader. `SkyLight.gi_intensity` scales it. Specular
   ambient (PBR) keeps the hemisphere.

The probes store diffuse irradiance, so a plain sky reads a little less contrasty than the hemisphere
(`(s + g) / 2 +- (s - g) / 3` instead of `(s + g) / 2 +- (s - g) / 2`). Tests (`gfx_stack_tests`): the voxelizer is
deterministic and has no gaps, the probes show colour bleeding next to a red wall, converge from a wrong state,
reproduce an open sky, invalidate probes inside a closed box, and both tiers read them; switching back to the
hemisphere gives the old pixels. Measured on the host (shared box): a full update of the demo room 60 to 80 ms
once (the first frame), then about 1 to 2.5 ms a frame (the update-rate setting scales it); the lit shader costs about 8 extra
linear taps a pixel: the demo at 800 x 530 on the CPU backend goes from 175 to 278 ms in the base tier. That is why
it is a choice and off at Low. Screenshots: `gfx-gi-hemisphere.png`, `gfx-gi-probes.png` (`fe-gfx-shot --gi probes`).
Not done: toroidal voxel scrolling, probe specular / the sky atlas, shadows from local lights in the voxels.

## Anti-aliasing, ambient occlusion and reflections: the choices

Each technique is a choice next to the older one (Settings > Graphics, `graphics_settings.h`, text, Lua):
`aa` = off | fxaa | smaa | taa, `ao` = off | ssao | gtao, `ssr` = off | on. Low and Medium keep FXAA / SSAO / no SSR
(Low is bit-identical to before: the base tier smoke checksum `214c411e` and the demo PNG md5 did not change); High
takes SMAA, Ultra TAA + GTAO + SSR. All three are GPU tier passes (on the CPU backend they only run when the GPU tier is forced);
`features().ssr` and `aa_mode` / `ao_mode` say what runs. FXAA stays selectable after SMAA and TAA exist.

| Pass | What it does | Measured on the host (800 x 530, High, forced GPU tier) |
|---|---|---|
| FXAA | one cheap pass over edge pixels | 44 ms |
| SMAA | luma edges (threshold 0.1, contrast adaptation), blend weights from an area texture, neighbourhood blend over the edge pixels only | 80 ms |
| TAA | jittered camera (Halton 2,3, 8 frames), full size linear depth, resolve with reprojection + 3x3 neighbourhood clamp | 104 ms (with its depth pass) |
| SSAO | 12 taps | 21 ms |
| GTAO | half resolution, 2 slices x 3 steps per side, horizon integral; accumulated over frames while the camera and items stand still; then the same two blurs | 30 ms |
| SSR | half resolution, 12 step view space march, roughness cutoff 0.55, Fresnel / smoothness weight | 10 ms (+ a material pass of about 2 ms) |

**SMAA** is SMAA 1x in structure (edges, weights, blend) but not the reference tables: the area texture (32 x 32,
`smaa_area_texture`, generated at startup, post_stack.h) comes from a small analytic model (the contour as a line between
the end heights of the edge run; orthogonal patterns only, no diagonal or corner patterns), and the search is done by
the shader with up to 8 nearest taps per side (no search texture, so the weights shader takes more than 16 samples and
the host JIT runs it as an interpreter, but only on edge pixels because the others are skipped with a depth mask). The weights go into two
targets (one per axis) because the targets have no alpha. Four texture units are enough (weights: edges + area).

**TAA** resolves in the HDR encoded space (before bloom and tonemap). The reprojection comes from the depth and the previous
view-projection only: **no motion vectors**, so moving objects are left to the neighbourhood clamp (a little ghosting, history
blend 0.1 rising to 0.6 where the history disagrees) and per-object previous transforms are not used. The history is dropped when the
size changes, the mode changes, or the pixel was off screen last frame. **TAA drives the upscale**: with `render_scale` < 1 the
lit pass, depth, AO and SSR run at the internal size, the resolve reads the jittered internal image (bilinear for the colour,
nearest 3x3 for the clamp) and writes the full size image, so there is no CPU upscale (`RendererStats::taa_upscale`). Test: at 0.6 it is closer to
the converged full size TAA picture than a plain bilinear upscale.

**GTAO / SSR notes.** GTAO needs the normal and depth G-buffer of the AO pass; SSR adds a roughness / metal G-buffer pass (a third
geometry pass at half size, only with SSR on). SSR adds the reflected scene colour in the composite pass (it reflects what is on screen;
anything that leaves the screen or the depth falls back to the existing hemisphere / probe specular, nothing is subtracted from it). The
shaders had to be written around the 32 temporary registers (scalars packed into vec4s, one block per tap) and the host JIT limit of 16 texture samples:
GTAO uses 14 samples, SSR 16 + 12 depth taps (it also runs as an interpreter, on the glossy pixels only).
Screenshots: `gfx-smaa.png`, `gfx-taa.png`, `gfx-gtao.png`, `gfx-ssr.png` (`fe-gfx-shot --quality high --set aa=smaa --set ao=gtao --set ssr=on`).

## Cascaded sun shadows

`shadow_cascades` (1..3) splits the sun shadow by view distance (ends at 14% / 38% / 100% of the shadow
range for 3, 30% / 100% for 2). All cascades share one map of `shadow_resolution`: one cascade uses all of
it, two or three use an atlas of 2 x 2 tiles of half the size. Each cascade is a texel snapped sphere fit of
its slice (the radius in 2^(1/4) steps, the centre on whole texels and the depth in coarse steps, so a
rotating or drifting camera does not make shadows swim). A tile is redrawn only when its fit or the casters
that reach it changed (a far object moving does not redraw the near cascade). With `cascade_cache` the far
cascades also keep their fit for 2 / 4 frames while the camera stays inside it (moving casters in them lag up
to 3 frames). The lit shader picks the finest cascade that contains the pixel (|ndc xy| < 0.97, z < 0.97)
and reads that tile with its own bias, PCF radius and normal offset (`kCascadeBase` in shaders_hq.h lists the
frame data layout). `GpuTierStats` reports `cascades`, `cascades_drawn`, `cascades_reused` and
`cascade_texel` (meters per texel). The point / spot atlas is unchanged. There is no scissor yet, so a tile is
cleared by a full screen triangle under its viewport (depth: whole surface, it is scratch).

## Materials

Each mesh object has a material in the Inspector's Look section: color, Roughness, Metal, Unlit and Shadows. The editor passes these to `DrawItem` (`color`, `roughness`, `metallic`, `unlit`, `cast_shadows`).

| Setting | Base tier | GPU tier |
|---|---|---|
| Unlit (draws the plain color) | yes | yes |
| Roughness | no | yes |
| Metal | no | yes |
| Shadows (whether the object casts one) | no | yes |

The defaults (roughness 0.6, metal 0, lit, casts shadows) give the same picture as before materials existed.

## What the editor calls

- `set_settings(s)` on change (it is cheap; calling it every frame is fine).
  Settings set before `init()` are kept.
- `features()` says what actually runs (for the settings UI: grey out what
  the backend cannot do); `backend()` is the FarcadeGL backend string.
- `gpu_tier_stats()`: milliseconds per stage of the last frame.
- Output is read back as before (`pixels()`).

With the GPU tier off and no fog the old code path runs unchanged (same
pixels bit for bit).

## Pipeline (all effects on)

| # | pass | target | notes |
|---|------|--------|-------|
| 1 | shadow | S x S color (packed depth) + own depth | casters from the strongest shadowing directional light; skipped when nothing changed |
| 2 | depth, normal | half size color + depth | linear depth (24 bit packed) and view space normals of all items |
| 3 | ssao | half size | Alchemy style, 12 spiral samples, `ao_radius`, `ao_strength` |
| 4 | blur x2 | half size | depth aware 5 tap, horizontal then vertical |
| 5 | lit | scene (HDR encoded) or final | items + sky; PBR, shadow lookup (PCF), AO, fog |
| 6 | bloom | 1/4 .. 1/64 | bright pass (threshold, Karis average), 13 tap downsamples, tent upsampling chain |
| 7 | composite | LDR or final | scene + bloom, then tonemap, grade, vignette, sRGB |
| 8 | fxaa | final (context color) | FXAA 3.11 style, two draws with a depth mask |

Targets are created lazily and recreated when the size changes; shaders are
generated per feature set and cached. Only stages that are on run; without
bloom the lit pass writes straight to the final (or FXAA input) target.

### Encodings (targets are 8 bit XRGB)

- **HDR**: `sqrt(clamp(c, 0, 8) / 8)` per channel, with exposure and tint
  applied when encoding ("pre-exposure"); decode is `e * e * 8`. Tonemapping
  happens in the composite pass. Values above 8 clip.
- **Depth** (shadow map, SSAO): 24 bit fixed point in r:g:b, cleared to white.
  Shadow maps are fetched with nearest filtering and filtered by hand (PCF).
- **AO + depth**: r = occlusion, g:b = 16 bit depth for the blur.

### Shadows

Orthographic light camera fitted to the view frustum slice
`[near, min(shadow_distance, farthest receiver)]` (bounding sphere, replaced by
the scene sphere when smaller). The radius is quantized in 2^(1/4) steps and the
center snapped to whole texels in light space, so the map does not shimmer.
Bias: normal offset in the vertex shader, a constant, and receiver plane depth
bias per PCF tap (from the normal; FGSL has no derivatives). `soft_shadows` =
10 Vogel disk taps rotated per pixel. On the CPU backend the map is capped at
1024 (clearing 4096 x 4096 costs about 60 ms).

### PBR

GGX distribution, Smith-Schlick visibility (k = (r+1)^2/8), Schlick Fresnel with
F0 = mix(0.04, albedo, metallic), diffuse scaled by (1 - F) and (1 - metallic).
Light intensities keep the base tier's meaning (Lambert without 1/pi), so the
specular lobe is multiplied by pi. Ambient: hemisphere light from `SkyLight`;
specular ambient uses the hemisphere color along the reflection vector,
blurred towards the average with roughness, times Karis' analytic split-sum
approximation. AO multiplies ambient fully and direct light by 40%.

### Fog

`exp(-0.06 * Sky.fog * distance)` towards the sky's horizon color (the flat
color in `SkyMode::Color`), applied to lit and unlit items before post.

## Shader compiling and the shader cache

FarcadeGL keeps every compiled program in `0:/cache/shaders` (the SDK's
`fglcache.h`, shared by all programs on the system), so a game's second
start, or switching the editor's viewport back to FarcadeGL, skips the
compiler. The limit follows the size of the 0: volume: 20 MB under 256 MB,
5 GB under 80 GB, 30 GB under 300 GB and 50 GB above, never more than half
the volume; the least recently used programs go first. Settings > Graphics
shows its size and has Clear cache and Recompile shaders.

The renderer compiles at most ~25 ms of shaders a frame
(`Renderer::set_compile_budget`): a frame that needs more stops early,
`compiling()` is true, and the next frame goes on. The editor keeps the
last picture with a "Compiling shaders  n / m" bar (m is how many the last
time needed), the player shows a loading box. Fixed-step player runs
(`--frames`) compile at once, so their frames stay reproducible.

## Limits hit (FGSL / FarcadeGL / host JIT)

- 32 vec4 temporaries per stage and none are shared between variables of a
  scope: shaders use blocks and vec4 packing (see comments in shaders_hq.cpp).
- 64 uniform vec4 registers: the GPU-tier lit shader now keeps camera and
  per-object values in uniforms and packs light records plus shadow matrices
  and metadata into a read-only storage buffer. This frees uniform space but
  does not raise the current 8-light shader-variant limit; lighting loops are
  still unrolled and bounded by instruction and temporary-register limits.
- The host JIT takes 16 texture samples per shader at most (else it falls back
  to the interpreter, 10x slower), turns `if` into predication (no work is
  skipped) and takes at most 512 distinct constants. Hence the 12 AO samples,
  the FXAA split in two passes, and depth masks (the depth test runs before the
  fragment shader) to skip sky pixels in SSAO and non-edge pixels in FXAA.
- Linear texture fetches cost about 2.4x nearest on the host.

## Costs (host CPU backend, 800 x 530, ultra, one core)

Roughly: shadow map 5 to 15 ms when it has to be redrawn (otherwise 0), depth
and normal 4, SSAO 15, blur 9, lit 60 (base tier: 30), bloom 10, composite 25,
FXAA 28: about 150 to 160 ms per frame. On a card these are small.

Things that will differ on a real GPU: bilinear/nearest costs, the depth-mask
trick (real hardware also rejects early), `gl_FragCoord` rounding, the exact
packing round trip (the host rounds to nearest when storing 8 bit values), and
`discard` handling in the mask passes.

## Tools

`fe-gfx-shot out.png [w h] [--quality low|high|ultra]` renders the demo scene
(high and ultra force the GPU tier on the CPU backend).
