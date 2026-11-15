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
