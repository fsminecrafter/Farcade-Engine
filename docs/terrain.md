# Terrain, streaming, foliage and occlusion culling

A **Terrain** is a scene object (**Create > Terrain**). Its ground is a
heightfield split into square chunks. You shape it with brushes in the
viewport and paint foliage onto it. At run time its chunks load and unload
around the camera, together with the objects that stand on them.

The code lives in these files:

- `engine/scene/terrain.{h,cpp}`: the heightfield, the brushes, undo steps,
  chunk meshes, the foliage scatter and the `.fterrain` file.
- `editor/src/terrain_bridge.{h,cpp}`: the data store, streaming, chunk
  detail and foliage batches. The editor, Play and the player all use it.
- `engine/gfx/culling.{h,cpp}`: frustum and occlusion culling in the renderer.

## Making a terrain

**Create > Terrain** makes a flat 256 × 256 m terrain at y 0, centred on the
world origin. The map is 8 × 8 chunks of 32 m, each with 32 × 32 cells of 1 m.

The terrain sits at its object's position, which is its first corner.
Rotation and scale are not used, so the Inspector hides them.

The Inspector's **Generate** section makes a new flat map. You choose:

| Field | Meaning |
| --- | --- |
| Y level | The height of the flat ground (world y) |
| Map size | Width and depth in meters, rounded up to whole chunks |
| Chunk size | 16, 32 or 64 m a side |
| Chunk detail | 16, 32 or 64 grid cells a chunk side |

A map has at most 4096 cells a side. The line under the fields shows the
chunk count, the cell size and the memory the heights take.

**Generate** asks before it replaces existing ground. It keeps the map
centred where it was. Undo (Ctrl+Z) brings the old map back.

## The brushes

The **Terrain tools** section has six tools. Select the terrain, choose a
tool, then hold the left mouse button in the viewport. The right and middle
mouse buttons still move the view.

A circle on the ground shows the brush. The inner circle is where it works at
full strength. While a tool is active, the move gizmo and click-to-select are
off.

| Tool | Left mouse | With Shift |
| --- | --- | --- |
| Add | Raises the ground (up to radius / 2 m a second at full strength) | Lowers it |
| Remove | Lowers the ground | Raises it |
| Smooth | Evens out bumps (towards the average of the neighbours) | |
| Level | Flattens to **Level height**. Ctrl-click takes the height under the mouse | |
| Delete | Cuts holes: no ground is drawn and bodies fall through | Fills the holes back in |
| Foliage | Paints the chosen foliage layer up to **Density** | Erases it |

**Radius**, **Strength** and **Hardness** set the brush. Hardness 0 fades
out all the way from the centre to the edge, and 1 gives a hard edge.

Each stroke, from mouse down to mouse up, is one undo step. The terrain
keeps only the chunks a stroke touched. Undo and redo follow the editor's
own history: the scene keeps the data's revision (`revision` in the terrain's
settings), and the data moves to whatever revision an undo brings back.

**Saving** the scene also writes each changed terrain's data to its
`.fterrain` file (by default `terrain/<name>_<id>.fterrain` in the project).
A new terrain never takes the name of a file that already exists. A
duplicated terrain gets a copy of the data under its own name.

## Settings (Inspector, Lua `obj.terrain`)

| Field | Lua | Meaning |
| --- | --- | --- |
| Load distance | `load_distance` | Meters from the camera to a chunk's edge within which the chunk is loaded at run time |
| Stream in editor | `stream_in_editor` | The editor view also unloads chunks and objects (off: the editor shows everything) |
| Detail distance | `lod_distance` | Full mesh detail up to here. Then half the cells, a quarter, an eighth, one step per doubling of the distance |
| Texture, Color, Roughness, Texture size | `texture`, `color`, `roughness`, `uv_scale` | The ground's look. Texture size is meters per repeat |
| Cast shadows | `cast_shadows` | |
| Friction, Layers | `friction`, `layer` | Physics |

Chunks of different detail meet without gaps: every chunk has skirts, short
walls down its edges that hide the cracks.

## Streaming: dynamic, always loaded, unloaded

Every object has a **Streaming** property, set in the Transform section.

| Mode | At run time |
| --- | --- |
| Dynamic (with its terrain chunk) | Loaded while the terrain chunk under it is loaded. An object over no terrain is always loaded |
| Always loaded | Never unloaded |
| Unloaded (a script loads it) | Not loaded until a script sets `obj.loaded = true` |

Streaming applies in Play and in exported games. In the editor it applies
only to terrains with **Stream in editor** on, and Unloaded objects stay
visible so you can edit them.

An unloaded object is not drawn. Its scripts and physics keep running: only
drawing is streamed.

The scene file writes the property as `streaming always` or `streaming
unloaded`. Dynamic is the default and is not written.

## Foliage

A terrain has four foliage layers, in the **Foliage** section. Each layer
grows a `.mof` model wherever you paint it with the Foliage tool.

| Field | Meaning |
| --- | --- |
| Name | What scripts find the layer by: `fe.find_foliage("grass")`, `fe.assign_shader_to_foliage("grass", path)` (docs/scripting.md). Several layers, on one terrain or many, can share a name |
| Models | One or more `.mof` models (a tuft of grass, flowers, small plants). Each instance takes one of them at random. Add them with **Add models...** (Ctrl-click or Shift-click several in the picker, Ctrl+A for all), **Add folder...** (every `.mof` in it), or drop files or folders from Files on the list. Their textures and material colours are used |
| Density | Instances per square meter where the layer is painted at full strength |
| Scale | The smallest and largest random scale |
| Distance | The farthest the layer is drawn. Over the last 60% it thins out |
| Max slope | Ground steeper than this gets none |
| Align to slope | The model stands along the ground's normal (else straight up) |
| Shadows | The layer casts shadows (off by default: it is expensive) |
| Shader | A custom `.fshader` for every instance, such as a wind sway (see below). Empty: the models' own textures and colours |

In the scene file and in Lua (`obj.terrain.foliage1_model`) the models are one
string of paths separated by `;` (at most 1023 characters, enough for about 20
to 40 models).

Each instance stands on the ground and is turned at random around its up
axis. It is never tipped over, so nothing ends up in the ground.

Instances are not stored. They are placed again from the painted density
with a fixed hash, so the same paint always gives the same grass, and the
`.fterrain` file only holds one byte per cell per layer.

### A shader for a layer (wind sway)

A layer's **Shader** is a `.fshader` (docs/graphics.md), like a mesh's Shader
slot. For foliage it is usually a `@displace` hook: a function that moves each
vertex, which the renderer runs in every pass that draws the instances
(shadow maps, depth, normals, lighting). The grass, its shadows and its
ambient occlusion all sway together, and every GPU tier effect stays on.
Hooks get `u_time` (x: seconds rendered, y: the frame's delta, z: the frame
number). `assets/samples/foliage/sway.fshader` is a ready sway: copy it into
the project and put it in the Shader slot, or from a script:

```lua
fe.assign_shader_to_foliage("grass", "shaders/sway.fshader")
```

The sample bends each instance more towards its tip, using the texture's v
coordinate as the height (v = 1 at the base, v = 0 at the tip; its comments say
how to flip it). Instances are merged into one mesh per block, so the hook
sees terrain space positions; the sample uses them to give neighbours
different phases.

A swaying layer's shadows are redrawn every frame (a still layer's cascades
are cached), and its batches are not used as occluders.

### How millions of instances are drawn

FarcadeGL has no instanced draws, so foliage has its own path:

- **Batches.** Each loaded chunk is split into blocks of about 16 m. Per
  layer, model and model part, each block's instances are merged into one
  mesh, so a block costs one draw per part of each model it uses.
- **Thinning by distance.** A block's instances are in random priority
  order. The renderer draws only a prefix of the batch
  (`RenderMesh::draw_count`), and the prefix shrinks with the block's
  distance. The whole batch is uploaded once and only the count changes.
  Batches are rebuilt only when the needed count passes a power of two.
- **Culling.** Blocks outside the view are culled. Blocks hidden behind
  hills are skipped by occlusion culling.
- **Bounded work.** Batches are built a few per frame, about 6 ms; the rest
  follow in the next frames. Batches unused for a while are dropped over a
  memory budget of 384 MB.

The Profiler tab shows the chunks drawn at each detail level, the unloaded
chunks and objects, and the foliage instances, batches and memory.

There is no alpha cut-out yet. Grass should be modelled blades, not textured
cards with transparent parts.

## Occlusion culling

**Settings > Graphics > Occlusion culling** is on by default. Every frame,
the renderer draws big nearby items into a small CPU depth buffer, 256
pixels wide. It then tests every other item's box against that buffer
through a max-depth pyramid. An item whose box is behind the occluders at
every pixel is not drawn.

- **What occludes.** Terrain chunks occlude with a coarse 8 × 8 mesh whose
  vertices take the lowest height around them, so the mesh never sticks out
  above the real ground. Other meshes (primitives, small models) occlude
  with their own triangles when they cover enough of the screen.
- **What never occludes.** Foliage, items with a custom shader, and items
  with `DrawItem::occluder` off.
- **The camera passes only.** Culling applies to the camera's passes (depth,
  G-buffer, lighting). Shadow maps still get every caster, because things
  outside the view can cast shadows into it.
- **Frustum culling is always on.**

The Profiler shows the items given, those outside the view, those hidden,
the occluders and the culling time.

For the renderer, `RenderMesh` has three related fields:

| Field | Meaning |
| --- | --- |
| `has_bounds` | The mesh supplies its own box (no vertex scan) |
| `occluder_positions` | A mesh to occlude with instead of the item's own triangles |
| `draw_count` | Draw only the first N vertices |

`DrawItem` has `occluder` and `foliage`. Foliage is never an occluder, does
not hide particles, and stays out of the probe GI.

## Physics

The physics step (`PhysicsWorld::set_terrains`) treats each terrain as a
static body. Each step, it tests a dynamic body's or character controller's
lowest points against the height under them: box corners, cylinder rims,
hull vertices, and sphere and capsule ends. The deepest four become a
contact manifold.

Bodies rest and roll on the ground, characters are grounded on it, and holes
let things fall through. A first contact is a collision with the terrain
object, so scripts get it in `on_collision`.

## Scripts

| | |
| --- | --- |
| `fe.terrain_height(x, z)` | The ground's world height and normal at x, z (`local h, n = fe.terrain_height(x, z)`), or nil when no terrain is there or it is a hole |
| `obj.streaming` | `"dynamic"`, `"always"` or `"unloaded"`, read and write |
| `obj.loaded` | Read: whether the object is loaded (an Unloaded object only after a script loaded it). Write `true` / `false` to load or unload an Unloaded object |
| `obj.terrain` | The terrain's settings (the table above) |
| `fe.find_foliage(name)`, `fe.assign_shader_to_foliage(name, path)` | Foliage layers by name (docs/scripting.md) |

## The `.fterrain` file

The file is little endian:

1. The header: `"FETR"`, version 1, chunks x and z, cells per chunk, chunk
   size (float), revision, and a bit per foliage layer that is present.
2. The heights: `(chunks_x * cells + 1) * (chunks_z * cells + 1)` floats,
   row by row along x.
3. The holes: one byte per cell, run length coded (a byte, then a varint
   count).
4. Each present foliage layer: one density byte per cell, run length coded.

A 256 m map with 1 m cells is about 260 KB when the foliage is painted in
patches.
