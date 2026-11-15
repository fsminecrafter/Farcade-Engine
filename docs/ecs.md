# The entity system (engine/ecs)

Runtime objects of the engine live in a flecs v4 world (vendored unmodified in
`external/flecs`, MIT). `engine/ecs/ecs.h` wraps it: game and editor code
include only `ecs/ecs.h` (it does not include `flecs.h`); flecs is used inside
`engine/ecs/*.cpp`. Entity ids are `fe::Entity` (`uint64_t`, a flecs id, 0 =
none/root).

```cpp
fe::World world;
world.init();
fe::Entity cam = world.create("Camera");               // has Transform + WorldTransform
world.add(cam, fe::ModuleKind::Camera);
world.set_current(cam, fe::ModuleKind::Camera);
world.progress(dt);                                      // runs the frame
world.shutdown();
```

## World API

| Area | Calls |
|---|---|
| entities | `create(name, parent)`, `destroy` (with all descendants), `alive`, `lookup(name)` (roots), `lookup_child(parent, name)`, `name`, `set_name` |
| hierarchy | `set_parent(child, parent)` (0 = root; refuses cycles and sibling name clashes), `parent`, `children(parent, out, max)`, `child_count` |
| transform | `transform(e)` (local, writable), `world_transform(e)` (computed), `update_transforms()`, `add_renderable`, `renderable`, `remove_renderable` |
| modules | `has/add/remove/get/get_mut/set(e, ModuleKind, ...)`, `modified`, `load(e, ModuleSet)`, `store(e, ModuleSet*)` |
| fields | `get_field/set_field(e, kind, "name", FieldValue)` |
| current | `set_current`, `clear_current`, `current(kind)` |
| frame | `progress(dt)`, `add_system(Phase, fn, user)`, `remove_system` |

Names are unique among siblings (a duplicate makes `create` return 0 and
`set_name`/`set_parent` return false); `.` in a name is plain text. The root
names `fe` and `flecs` belong to the engine.

## Components

All registered with their size/alignment and with flecs **meta** (reflection),
under the scope `fe.`:

- `Transform { Vec3 position; Quat rotation; Vec3 scale; }`: local, relative to
  the parent. `WorldTransform { Mat4 matrix; }`: computed (column-major, as
  `Mat4`). Every entity made by `create` has both. `Renderable { uint8_t mesh;
  char model_path[256]; Vec3 color; bool visible; }`: optional; `mesh` is the
  editor's MeshKind index.
- Every `ModuleKind` of `scene/modules.h` (`fe.rigidbody3d`, `fe.light`,
  `fe.camera`, ...), registered from `module_info(kind)`. Components are
  created with the struct's defaults (a ctor hook copies a default instance).
  Meta types come from the `FieldInfo` tables: Float to f32, Int to i32, Bool to
  bool, Enum to a flecs enum (`fe.enums.<module>_<field>`, u8, constants named
  like `enum_names`), Vec3 and Color to the shared struct `fe.Vec3 {x,y,z}`, Path
  to `char[256]`, Bits to u32. Members have the struct's own offsets, so the
  flecs meta cursor, JSON and the explorer see the same data as the C++ struct.
  Adding a module or a field in `modules.cpp` is all that is needed.

## The "current" rule

Camera, SkyLight, Sky, PostProcess and AudioListener have a `current` flag
(`ModuleInfo::has_current`). At most one entity per kind has it; whoever turned
it on last wins. It is enforced by a flecs **OnSet observer** per kind, so every
way of turning the flag on goes through it: `set_current`, `set_field(...,
"current", true)`, `set()`, `load()`, or `get_mut()` followed by
`modified(e, kind)`. (Without `modified`, flecs cannot know the data changed:
that is the one rule for writing through `get_mut`.) `current(kind)` is 0 when
none is; destroying or removing the current one just leaves none. Other kinds
reject it: `set_current` returns false, they have no `current` field.

## Frame and phases

`World::progress(dt)` runs the flecs pipeline in this order:

| Phase | For | Notes |
|---|---|---|
| `PreUpdate` | input, script start/pre-frame | |
| `Update` | **scripts, physics**: they write `Transform` | the plug-in place for later systems |
| `PostUpdate` | transform system (first), then callbacks | `WorldTransform` is computed from `Transform` parents-first (a flecs cascade query over ChildOf) |
| `Store` | **audio, render extraction**: read `WorldTransform` | |

`add_system(Phase, fn, user)` adds a `void fn(World&, float dt, void* user)`
that runs once per frame in that phase. Inside such a callback reads and field
writes are immediate, but structural changes (create, destroy, add/remove a
module, `set_parent`) are deferred by flecs to the end of the phase.
Engine-internal systems that need queries use `world.raw()` and `component(kind)`
with `flecs.h`.

## How scripts reach fields

The Lua bindings (later) hold an entity id and call
`get_field(e, kind, "intensity", &v)` / `set_field(e, kind, "intensity", v)`
where `kind` comes from `module_by_name("light")` (the `ModuleInfo::name`, the
Lua name). Values are validated against the `FieldInfo`: numbers convert and
clamp to the range, enums take an index or the constant's name, strings are
cut to 255 chars, a wrong kind of value or an unknown field returns false.
Setting `current` goes through the rule above.

## Build

`mk/ecs.mk` defines `ECS_SRC` (`engine/ecs/world.cpp`), `FLECS_SRC`
(`external/flecs/flecs.c`, C, warnings off) and the flags. flecs is built with
`-DFLECS_NO_OS_API_IMPL -DFLECS_CUSTOM_BUILD -DFLECS_SYSTEM -DFLECS_PIPELINE
-DFLECS_META`, plus `-D__linux__` on Minimal-OS; `world.cpp` installs the
(single-threaded) OS API: libc malloc, `clock_gettime`, log to stdout. Tests:
`build/host/bin/ecs_tests` (`make test`); on Minimal-OS
`build/os/bin/ecs_smoke.run` (`tools/ci/run_os.sh build/os/bin/ecs_smoke.run
'FE ECS PASS' 'FE ECS FAIL'`). Checks shared by both are in
`tests/common/ecs_checks.h`.
