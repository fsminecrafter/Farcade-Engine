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
| 6 | bloom | 1/4, 1/8, 1/16, 1/32 | bright pass (threshold), 4 downsamples, tent upsampling chain |
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
