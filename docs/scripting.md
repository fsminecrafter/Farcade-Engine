# Scripts on objects

An object can hold several scripts (up to 6), which run in the order the
Inspector lists them. A script declares **inputs** that the Inspector shows
and the scene saves for each object, so the same `door.lua` can sit on two
doors with different settings. An **object** input links to another object,
and through that link a script reaches the other object's scripts. That is
how scripts on different objects work together.

Status: the editor side is done (the data model, reading declarations, the
Inspector, links that survive duplicate and delete, Play/Stop). Scripts do
not *run* yet: that needs the Lua 5.4 runtime (plan C5). The run-time API
below is the design the runtime will implement. Its editor-side parts
already exist in C++ (`Scene::find_script`, `Scene::script_prop`).

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
```

| Type | Inspector | Default | Lua value |
| --- | --- | --- | --- |
| `number` (`float`, `int`) | drag field, or a slider with `range a b` | `= 5` (0) | number |
| `text` (`string`) | text field (63 bytes) | `= "..."` or bare words ("") | string |
| `toggle` (`bool`) | switch | `= true` / `false` (false) | boolean |
| `list(a, b, ...)` | drop-down, up to 8 choices | `= b` (the first) | the chosen string |
| `object` | object picker; drop a Hierarchy row on it | none | object handle or nil |

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

## At run time (planned, with the Lua runtime)

Each script on each object gets its own instance table `self`. Its inputs
are fields of `self`, and `self.object` is the object it sits on.

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

The API scripts use to reach each other:
- `obj:script(name)`: the first script called `name` (`"door"` or `"door.lua"`) on `obj`, or nil. Through it you read and change its inputs and state and call its functions.
- `obj:scripts()`: all of them, in order.
- `obj:send(event, ...)`: calls `on_<event>(self, ...)` on every script of `obj` that defines it.
- `fe.broadcast(event, ...)`: does the same on every object (the shared event bus of plan C5).
- `fe.find(name)`: an object by name, for the cases an `object` input doesn't cover.

Scripts on one object run in Inspector order each frame, and objects run in
Hierarchy order. A message sent with `send` is delivered right away, inside
the sending script's call.
