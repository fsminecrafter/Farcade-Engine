# Scene files

The editor saves a scene as a `.fscene` file: plain text, one value per
line, written and read by `editor/src/scene_io.{h,cpp}`. Two objects from
the demo scene, as the editor saves them:

```
farcade-scene 1
next_id 14

object 4
  name "Pillar"
  kind mesh
  position 2.5999999 1 1.60000002
  rotation 0 0 0 1
  scale 0.600000024 2 0.600000024
  mesh cylinder
  color 0.75 0.75999999 0.800000012
  roughness 0.300000012
  metallic 0.899999976
  unlit false
  cast_shadows true
  module collider3d
    shape Cylinder
    mode Collision
    center 0 0 0
    size 1 1 1
    radius 0.600000024
    height 1
    friction 0.5
    bounciness 0
    layer 1
  end
end

object 6
  name "Lamp"
  kind mesh
  parent 4
  position 0 0.649999976 0
  rotation 0 0 0 1
  scale 0.600000024 0.150000006 0.600000024
  mesh sphere
  color 0.980000019 0.850000024 0.449999988
  roughness 0.600000024
  metallic 0
  unlit false
  cast_shadows true
  script "scripts/lamp.lua"
    prop follow object 3
    prop threshold number 6 0 0
  end
end
```

- **The header** `farcade-scene <version>` comes first. A file from a newer
  version is refused.
- **Objects** keep their ids. Parents (`parent`) and script inputs that
  link to objects (`prop <name> object <id>`) refer to them.
  `next_id` makes sure new objects never reuse an id.
- **Modules** are written field by field, using the same reflection tables
  as the Inspector and Lua (`engine/scene/modules.h`). Enum values are
  written by name.
- **Script inputs** store their value. List inputs store the chosen text
  and the choices.
- **Floats** are written with 9 significant digits, so they read back
  exactly.
- **Paths** inside the project are stored relative to it. This covers
  textures, models, scripts and audio.
- **Terrains** (`kind terrain`) keep their heights, holes and foliage in their
  own `.fterrain` file, which the `terrain` module's `data` field names and
  which is saved with the scene. `revision` ties the file to the editor's undo
  history (docs/terrain.md).
- **Streaming** is written as `streaming always` or `streaming unloaded`.
  Dynamic, the default, is not written.
- **Groups** are written as `groups "grass;sway"` (names separated by `;`),
  and only when the object has some.

## Reading rules

- **Unknown names are skipped and counted.** A name can be unknown at
  any level: an object line, a module, or a module field. The editor
  logs how many lines were skipped and the first one.
- **Missing values keep their defaults.** A field added later therefore
  reads back as its default from an older file.
- **These files are refused:**
  - a file without the header;
  - an object id used twice;
  - a file that ends inside an object.

  The open scene is left unchanged.
- **Broken links are cleaned up:**
  - a parent or object link to a missing id is cleared;
  - parent loops are cut.
- **More than one current object of a kind** (camera, sky, ...): the
  first one in the file keeps the flag.

## In the editor

| Action | Effect |
|---|---|
| File > Save scene (Ctrl+S) | Saves to the open file. If the scene was never saved, it goes to `scenes/main.fscene`. |
| File > Save scene as... | Asks for a name and saves to `scenes/<name>.fscene`. |
| Double-click a `.fscene` in Files | Opens it. |
| File > New scene | Empties the scene. |

## Selecting and editing several objects

| Action | Effect |
|---|---|
| Shift-click (viewport or Hierarchy) | Adds an object to the selection, or takes it out |
| Shift + double-click an object | Selects every object like it: meshes with the same model (or the same primitive shape), empties with the same modules (lights also of the same type), special objects of the same kind. Hidden objects are skipped, and the one clicked stays active |
| Click a group chip (Inspector, Groups) | Selects every object in that group; Shift-click adds them |
| Hierarchy filter | Matches names and groups |

With several objects selected, the Inspector shows the active one (the last
picked) with "+N more: edits apply to all". A change made there goes to every
selected object that has the same thing: only the fields that changed are
copied, so the other values stay each object's own.

- Transform: rotation, each axis of the scale separately, the parent,
  Streaming and Visible. Moving the gizmo moves them all.
- Look and Mesh: to the other mesh objects.
- Modules: an added or removed module, and each changed field (a vector
  per component). A special object's settings (Camera, Sky ...) go only to
  objects of the same kind. The Current flag and a terrain's data file stay
  each object's own.
- Script inputs: to objects with the same script.
- Groups: the groups added or removed; each object keeps its others.

One edit is one undo step for all of them.

- **Saving during Play** writes the scene as it was before Play.
- **Opening a scene** stops Play first.
- **Script inputs after opening:** they are read from the script files
  again, and the saved values are kept.
- The title bar shows the project and the open scene.

## Unsaved changes, undo and redo

**Unsaved changes.** New scene, opening a scene (from Files) and Quit ask
first when the scene differs from its last save, open or new. The choices
are Save, Don't save and Cancel. Save writes the open file first; if
writing fails, nothing else happens.

**Undo and redo:**

| Action | Keys |
|---|---|
| Undo | Ctrl+Z, or Edit > Undo |
| Redo | Ctrl+Y or Ctrl+Shift+Z, or Edit > Redo |

How it works:
- **Snapshots.** The history keeps whole-scene snapshots: the scene file
  text, without `next_id` and the hierarchy's folding.
- **When a snapshot is taken.** After a click, key or text input, once no
  mouse button is down and no field is being edited, and only if the
  scene changed. So a gizmo drag or a slider drag is one step.
- **Size.** Up to 100 steps.
- **What is kept on undo.** The selection (where the objects still exist)
  and the hierarchy's folding. Ids are never reused.
- **Play.** There is no undo while playing. Play itself adds no steps,
  since Stop puts the scene back.

On Minimal-OS, `farcade-editor.run --scene-test` saves the demo scene
twice, opens it again and prints `FE SCENE PASS <objects> <bytes>` when it
comes back unchanged.

## Models in scenes

An object with `mesh model` and a `model "<path>.mof"` line shows an imported
model. `.mof` files are read through the VFS (`engine/core/vfs.h`): plain files
under the project in the editor, the package in an exported game. A file that
cannot be read shows as a cube.

**Importing:** Create > Import model... (a path to a `.gltf`, `.glb` or `.obj`),
the Files panel's "Import as model" (right click, or double-click the file),
and dropping such a file on the viewport or an object all convert it with
the engine's importers to a `.mof` next to the source and use that. The
converter is `model_convert_file` (`editor/src/model_import.h`).

**What is drawn** (`editor/src/model_cache.h`): every mesh instance of the node
tree, one part per submesh, in the bind pose with the node transforms applied.
FarcadeGL draws a part with its material's base color factor (the object's
color tints it; the default grey is neutral) and its base color texture (PNG,
JPEG or raw RGBA8 stored in the `.mof`, mip 0) with the file's UVs. The CPU
preview draws the merged mesh in one color. Bounds, picking, framing and
Mesh colliders use the real mesh.

**For other code** (an Animator module, say):

```cpp
const fe::Model* m = fe::Scene::model_for(object);   // null unless a Model object whose file loaded
m->mesh                  // MeshData: all parts merged (triangle list)
m->parts[i]              // positions, normals, uvs (triangle lists, bind pose), color, texture,
                         //   mesh_index / submesh_index / material / node into m->mof
m->mof                   // the mof_file view: bones (inverse bind matrices), nodes,
                         //   mof_mesh_joints() / mof_mesh_weights() skin streams, materials, textures
```

The pointers stay valid until `model_cache_clear()` (called by Play, Import and
when a model file is assigned). To pose a skinned model, compute the bone
matrices from `m->mof.bones`, skin the `.mof` vertex streams
(`mof_mesh_positions` plus joints and weights of `parts[i].mesh_index`) and draw
the result as your own `gfx::DrawItem`s; the parts are the bind pose. `.mof`
carries no animation clips yet.
