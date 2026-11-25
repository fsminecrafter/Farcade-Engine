# Scripts on objects

An object can hold several scripts (up to 6), which run in the order the
Inspector lists them. A script declares **inputs** that the Inspector shows
and the scene saves for each object, so the same `door.lua` can sit on two
doors with different settings. An **object** input links to another object,
and through that link a script reaches the other object's scripts. That is
how scripts on different objects work together.

Status: scripts run. Press Play and every script on every object starts in
the Lua 5.4 runtime (`engine/script/runtime.h`), with its inputs and object
links. Scripts run before the physics step each frame. Trigger and collision
events reach them, and `print` and errors (with `file:line`) go to the
Console. Stop puts the scene back. Tested on the PC (`script_tests`, the
editor tests) and on Minimal-OS (`script_smoke`).

## Declaring inputs

Inputs are declared in comments, so the editor reads them without running
Lua. One per line, anywhere in the file:

```lua
-- @prop speed   number = 5  range 0 20   -- a slider from 0 to 20
-- @prop gain    number = 1.5             -- a number field (drag to change)
-- @prop label   text   = "Front door"    -- quoted, or the rest of the line
-- @prop mode    list(closed, open, locked) = locked
-- @prop active  toggle = true            -- true/false, on/off, yes/no
-- @prop target  object                   -- a link to another object
-- @prop skin    file                     -- any project file; drag one onto the slot
```

| Type | Inspector | Default | Lua value |
| --- | --- | --- | --- |
| `number` (`float`, `int`) | drag field, or a slider with `range a b` | `= 5` (0) | number |
| `text` (`string`) | text field (63 bytes) | `= "..."` or bare words ("") | string |
| `toggle` (`bool`) | switch | `= true` / `false` (false) | boolean |
| `list(a, b, ...)` | drop-down, up to 8 choices | `= b` (the first) | the chosen string |
| `object` | object picker; drop a Hierarchy row on it | none | object handle or nil |
| `file` (`asset`) | file slot; drop a file from the Files panel | `= "assets/image.png"` or empty | path string (project paths save relative) |

Rules:
- A script can declare up to 12 inputs.
- A line that does not parse is skipped and the console says so. Examples: an unknown type, an empty list, a default that is not one of the choices, a name used twice.
- Text after a declaration must be a `--` comment.

## When the file changes

The editor reads the declarations in three cases:
- when a script is added;
- when its ⟳ button is pressed;
- when Play starts.

Inputs are matched by name:
- An input still declared with the same type keeps the value set in the editor. A number is clamped into a new range, and a list choice is found again by its text.
- New inputs get their defaults.
- Inputs that are gone are dropped.
- An input whose type changed starts again from its default.
- If the file cannot be read, the inputs keep their values and the script shows "missing file".

## In the editor

- **Adding a script:** drop a script on an object (in the viewport or the Hierarchy) or on **Add script** in the Inspector. You can also click Add script to pick from the project's scripts (the project folder and `scripts/`). Each drop adds a script; the same file may be added more than once.
- **Each script's section:** open the file in the editor, read the inputs again, move the script earlier, or remove it. Then come its inputs.
- **Object inputs:** the picker shows how many scripts each object has. Hovering a linked object lists its scripts, and the arrow button selects it. The Inspector also says how many script inputs link to the selected object.
- **Duplicating:** a link that points inside the copied tree goes to the copy. A switch duplicated together with its door gets its own door. Links to objects outside the copied tree stay as they were.
- **Deleting:** removing an object clears every link to it.
- **Play and Stop:** Stop puts all inputs back as they were before Play.

## At run time

Each script on each object gets its own instance table `self`. Its inputs
are fields of `self`, `self.object` is the object it sits on and `self.name`
is the script's name ("door"). The script file runs once per instance, in an
environment of its own: the functions and globals it defines belong to that
instance, and `self` falls back to them, so another script can call
`door:open()`.

```lua
-- switch.lua
-- @prop door object
-- @prop mode list(press, hold) = press

function on_event(self, name)
  if name ~= "use" or not self.door then return end
  local d = self.door:script("door")      -- the door's door.lua instance
  if d and not d.locked then
    d:open()                              -- call a function defined in door.lua
  end
  self.door:send("used_by", self.object)  -- every script on the door gets on_used_by
end
```

```lua
-- door.lua
-- @prop speed number = 2 range 0 5
-- @prop locked toggle = false

function open(self) self.opening = true end
function on_used_by(self, who) print(who.name .. " used me") end
```

### Callbacks

All optional, `self` first. Scripts on one object run in Inspector order each
frame, objects in the order their instances were added (Hierarchy order).

| Function | When |
| --- | --- |
| `on_start(self)` | once, before the first `on_update`; every `on_start` of a frame runs before any `on_update` (a script added later starts on the next frame) |
| `on_update(self, dt)` | every frame (ECS phase Update) |
| `on_<name>(self, ...)` | `obj:send("<name>", ...)` or `fe.broadcast("<name>", ...)` |
| `on_event(self, name, ...)` | the same events, after `on_<name>`; an event called `event` only reaches `on_event` |
| `on_trigger_enter(self, other)`, `on_trigger_exit(self, other)` | an object entered or left a trigger collider on this object |
| `on_collision(self, other)` | this object's body hit another one (called on both objects) |

Inputs: Number is a number (an integral value is a Lua integer), Text a
string, Toggle a boolean, List the chosen string, Object a handle or nil,
and File a path string. Project paths are saved relative to the project. A File input can be used to assign an asset to any
object with a mesh:

```lua
-- @prop target object
-- @prop skin file
function on_start(self)
  if self.target then self.target.material.texture = self.skin end
end
```

What a script stores in `self` stays there.

### Input

Keyboard and mouse, in the editor's Play (over the viewport) and in the player:

| | |
| --- | --- |
| `fe.key_down(name)` | true while the key is held |
| `fe.key_pressed(name)`, `fe.key_released(name)` | true during the update after the key went down / up |
| `fe.mouse_x()`, `fe.mouse_y()` | the pointer, in pixels of the game's view |
| `fe.mouse_down(b)`, `fe.mouse_pressed(b)` | button 0 left, 1 right, 2 middle |
| `fe.mouse_wheel()` | scroll since the last update |

Key names: `"a"` to `"z"`, `"0"` to `"9"`, `"space"`, `"enter"`, `"escape"`, `"tab"`,
`"backspace"`, `"left"`, `"right"`, `"up"`, `"down"`, `"shift"`, `"ctrl"`, `"alt"`, `"f1"` to
`"f12"`, `"pageup"`, `"pagedown"`, `"home"`, `"end"`, `"insert"`, `"delete"`.

### Objects

`self.object`, `other` in the collision callbacks, `fe.find`, `obj.parent`
and `obj:children()` give handles. One handle exists per object, so `==` and
using a handle as a table key work.

| | |
| --- | --- |
| `obj.name` | read/write; renaming to a sibling's name is an error |
| `obj.position`, `obj.rotation`, `obj.scale` | local, read/write. `rotation` is Euler degrees (X, then Y, then Z, like the Inspector); `scale` also takes a number |
| `obj.world_position` | read only, as of the last transform update (the end of the previous frame) |
| `obj.parent` | handle or nil |
| `obj:children()` | array of handles (order unspecified) |
| `obj:script(name)` | `self` of the first script on `obj` called `name` (`"door"` or `"door.lua"`), or nil |
| `obj:scripts()` | array of all their `self` |
| `obj:send(event, ...)` | delivers right away, inside the call |
| `obj:move(dx, dy, dz)` | adds to `position` (also `obj:move(vec3)`) |
| `obj:destroy()` | a request: the object and its children are destroyed when the outermost callback returns. The handle reads `alive == false` from then on, and every other access raises "the object no longer exists" |
| `obj.alive` | false once destroyed |
| `obj.material` | the look of a mesh object, or nil for objects without a mesh (see below) |
| `obj.groups` | the object's groups as an array of names. Assign an array (`{"grass", "sway"}`) or a string (`"grass;sway"`) to replace them |
| `obj:in_group(name)` | whether the object is in that group |
| `obj:add_group(name)`, `obj:remove_group(name)` | add or remove one group; they return whether anything changed. A name is 1 to 63 characters without `;` |
| `obj.streaming`, `obj.loaded` | the object's Streaming mode (`"dynamic"`, `"always"`, `"unloaded"`) and whether it is loaded; `obj.loaded = true` loads an Unloaded object (docs/terrain.md) |
| `obj.<module>` | `rigidbody3d`, `collider3d`, `character_controller3d`, `listener`, `audio_stream`, `audio_sampler`, `light`, `particles`, `joint3d`, `animator`, `force_field`, `spawner`, `reverb_zone`, `billboard`, `text3d`, `camera`, `skylight`, `sky`, `post_process`, `terrain` (see modules.md; methods such as `obj.particles:emit(10)`, `obj.animator:play("walk", 0.3)`, `obj.spawner:spawn()`, `obj.joint3d:snap()` — `break` is a Lua keyword): the module, or nil if the object has none |

A module's fields read and write through the ECS (`World::get_field` /
`set_field`): numbers are clamped to the field's range, enums are their name
strings (`light.type == "Spot"`), colors and vectors are vec3. Writing an
unknown field or a value of the wrong type raises an error that names the
module and the field. `camera.current = true` goes through the one-current
rule; inside a frame the world applies that rule at the end of the Update
phase, so `fe.current("camera")` may still return the old camera until then
(from `trigger_enter` & co., called outside a frame, it is immediate).

`obj.material` reads and writes the object's look, the same settings as
the Inspector's Look section:

| Field | Type | Notes |
| --- | --- | --- |
| `color` | vec3 | |
| `roughness` | number | clamped to 0..1 |
| `metallic` | number | clamped to 0..1 |
| `unlit` | true / false | |
| `cast_shadows` | true / false | |
| `visible` | true / false | |
| `texture` | file path string | Image file path; assign `""` to clear |
| `shader` | file path string | Combined `.fshader` shader path; assign `""` to clear |

```lua
local m = self.object.material
m.metallic = 1
m.roughness = 0.2
m.color = fe.vec3(1, 0.8, 0.3)
m.texture = self.skin
```

- Roughness, metal and shadows show only with FarcadeGL's GPU tier. Color,
  unlit, visible, textures and shaders show with every renderer: a shader's
  `@displace` and `@surface` hooks run inside the GPU tier's passes, and a
  full custom shader draws its own object (docs/graphics.md).
- Changes made in Play are visible in the editor while playing and are put
  back on Stop, like everything else.
- A wrong type or an unknown field raises an error naming the field.
- `obj.material` itself is read only: change its fields instead.

Positions, rotations and colors come back as new vec3 values: assign the
changed vector (`obj.position = obj.position + v`), changing `.x` of the copy
does nothing.

### Groups

A group is a name you give objects in the Inspector (the **Groups** row of the
Transform section), so scripts can find several of them at once. An object can
be in several groups. Names match without regard to case (`"Grass"` finds
`grass`).

```lua
for _, o in ipairs(fe.find_group("lamps")) do
    o.light.intensity = 0
end
if other:in_group("enemy") then self.health = self.health - 10 end
self.object:add_group("collected")
```

Group changes made while playing are put back on Stop. Objects made while
playing (spawner copies) keep the groups of the object they copy.

### Foliage layers

`fe.find_foliage(name)` returns handles to terrain foliage layers by their
**Name** (set in the terrain's Foliage section). A handle reads and writes the
layer's settings:

| Field | |
| --- | --- |
| `name`, `models` | strings (`models` is the `;` separated list) |
| `density`, `scale_min`, `scale_max`, `distance`, `max_slope` | numbers |
| `align`, `shadows` | true / false |
| `shader` | a `.fshader` path, `""` for none |
| `object`, `layer` | the terrain object and the layer's number (1 to 4), read only |

```lua
-- Every grass layer sways in the wind (assets/samples/foliage/sway.fshader):
fe.assign_shader_to_foliage("grass", "shaders/sway.fshader")
-- The same, one layer at a time:
for _, layer in ipairs(fe.find_foliage("grass")) do
    layer.shader = "shaders/sway.fshader"
    layer.density = layer.density * 0.5
end
```

They are the terrain's `foliage<n>_<field>` settings, so
`obj.terrain.foliage1_shader` is the same value as a layer handle's `shader`.

### vec3

`fe.vec3(x, y, z)` (also `vec3(...)`, and `fe.vec3(t)` from a table) is a
table with `.x .y .z` and a metatable: `+`, `-`, `*` (by a number, or
component-wise by another vec3), `/` (by a number), unary `-`, `==`,
`tostring`, `v:length()`, `v:normalized()`, `v:dot(w)`, `v:cross(w)`,
`fe.dot(a, b)`, `fe.cross(a, b)`. Wherever a vec3 is assigned you can also
give `{x=, y=, z=}` or `{1, 2, 3}`.

### The `fe` library

| | |
| --- | --- |
| `fe.find(name)` | the root object called `name`, else the match nearest the roots; nil if none |
| `fe.find_group(name)` | every object in that group, as an array in Hierarchy order (empty when none). Names match without regard to case (see Groups below) |
| `fe.find_foliage(name)` | every terrain foliage layer called `name`, on every terrain, as an array of layer handles (docs/terrain.md) |
| `fe.assign_shader_to_foliage(name, path)` | sets the shader of every foliage layer called `name` (`""` or nil clears it); returns how many layers changed |
| `fe.current(module)` | the current camera/sky/skylight/post_process/listener object, or nil |
| `fe.broadcast(event, ...)` | `on_<event>` and `on_event` on every script instance |
| `fe.time()` | seconds since the runtime started |
| `fe.terrain_height(x, z)` | the ground's world height and normal there (`local h, n = fe.terrain_height(x, z)`), nil off the terrains and over holes (docs/terrain.md) |
| `fe.status(text [, progress])` | shows a line (and a bar when `progress`, 0..1, is given; without it the bar moves on its own) over the game, in the player and in Play; `fe.status()` hides it. For loading screens and "waiting for ..." messages; it is drawn together with FarcadeGL's "Compiling shaders" bar |
| `fe.dt` | the dt of the frame being run |
| `fe.log(...)`, `print(...)` | to the console (Info), arguments as text separated by tabs |
| `fe.warn(...)`, `fe.error(...)` | to the console as Warning / Error |

Scripts see the base functions (`pairs`, `pcall`, `setmetatable`, ...), `coroutine`,
`table`, `string`, `math` and `utf8`. There is no `io`, `os`, `package`,
`debug`, `require`, `dofile`, `loadfile` or `load`, and `collectgarbage`
only knows "collect", "count" and "step". Globals a script sets are its
instance's own, they are not shared with other scripts.

### Errors

Errors at load or run time are logged as `path:line: message` plus a traceback.
An instance that fails at run time gets no more callbacks (the others keep
running); one that fails to load is not added. A destroyed object's scripts
never run again.

### Running scripts from C++

```cpp
World world; world.init();
ScriptRuntime scripts; scripts.init(&world, host);       // adds its systems to the world
int id = scripts.add_instance(entity, "scripts/door.lua", text, size, props, nprops, linked_entities);
// each frame: world.progress(dt);  (physics calls scripts.trigger_enter/exit, collision)
```

The interpreter is Lua 5.4.7 (`external/lua`, unmodified) with a generational
garbage collector, built for the host and for Minimal-OS (about 200 KB of
code); the engine's own `setjmp`/`longjmp` shim and a C-locale stub fill the
gaps of the Minimal-OS C runtime (`engine/script/lua_os_compat.c`,
`engine/script/os_include`). `mk/script.mk` defines `LUA_SRC` and `SCRIPT_SRC`.

Not done: a limit on how long one callback may run (an endless loop in a
script hangs the frame).
