# Modules and scene objects

The data is defined in `engine/scene/modules.h`. Each struct there has a
field table in `modules.cpp`, and three parts of the engine read it:
- the Inspector draws its rows from it;
- the ECS registers it with flecs;
- the Lua bindings will use the same names.

## Modules

Modules attach to any object, at most one of each kind. In the Inspector
you add one with **Add module** and remove it with its trash button.

| Module | Lua name | What it is for |
| --- | --- | --- |
| Rigidbody 3D | `rigidbody3d` | Physics: dynamic, kinematic or static, mass, damping, gravity, start velocity |
| Collider 3D | `collider3d` | A box, sphere, capsule or mesh shape. *Collision* makes bodies bounce off it; *Trigger* reports objects entering and leaving (`on_trigger_enter` / `on_trigger_exit`) |
| Audio Listener | `listener` | Where the game hears from. Has **Current** |
| Audio Stream | `audio_stream` | A long sound streamed from a file (music, ambience), optionally positioned in 3D |
| Audio Sampler | `audio_sampler` | Short sounds held in memory and played by scripts, several at once |
| Light | `light` | Directional, point or spot light |

## Scene objects

These are object kinds with their own settings. You add them from
**Create** or the Hierarchy's + menu. They have no mesh: the viewport shows
them as an icon you can click, and cameras and lights also get their shape
drawn as lines.

| Object | Lua name | What it is for |
| --- | --- | --- |
| Camera | `camera` | Perspective or orthographic view along the object's −Z axis. Has **Current** |
| Sky | `sky` | The background: a zenith/horizon/ground gradient with a sun disc, or one flat color. Has **Current** |
| Sky Light | `skylight` | Ambient light from the sky and the ground. Has **Current** |
| Post Processing | `post_process` | Exposure, tonemapping, contrast, saturation, tint, vignette, bloom. Has **Current** |
| Empty | — | Only a transform: use it as a group, a marker, or a holder for modules |

Lights are Empty objects with a Light module (**Create > Light**).

## Current

For cameras, skies, sky lights, post processing and listeners, only one
object in a scene is **current** at a time. That object is the camera the
game renders with, the sky that is drawn, or the listener that hears.

- **In the editor:**
  - The Current switch is the first row in the object's settings.
  - Turning it on takes it from the object that had it, and the console says so.
  - The Hierarchy marks current objects with a star.
  - The first object of each kind becomes current when it is created.
- **Duplicates and deletes:**
  - A duplicate is never current.
  - Deleting the current object leaves none current. The renderer then uses default settings.
- **Play mode:** Play/Stop puts the flags back as they were before Play.
- **In C++:** `Scene::set_current`, `clear_current`, `current(kind)` and `is_current` in the editor. The ECS has the same rule (`World::set_current`), enforced however the flag is set.

Scripts reach Current like any other field. The planned Lua API (it arrives with the Lua runtime):

```lua
-- switch to the security camera when the player enters a trigger
-- @prop security object

function on_trigger_enter(self, other)
  if other.name == "Player" then
    self.security.camera.current = true      -- the old camera stops being current
  end
end

function on_update(self, dt)
  local cam = fe.current("camera")           -- the object, or nil
  if cam then cam.camera.fov = 60 + 10 * math.sin(fe.time()) end
  self.object.light.intensity = 2             -- modules by their Lua names
end
```

## What runs today

- **Done in the editor:** the data model, the Inspector, creating objects, the Current rule, and the viewport icons and shapes.
- **Physics:** Rigidbody and Collider settings are saved, but nothing simulates them yet (plan C6: Jolt).
- **Audio:** Audio settings are saved, but nothing plays them yet (plan C6: the engine's mixer over `SYS_AUDIO_STREAM`).
- **Rendering:** the editor's CPU preview does not use lights, sky, sky light or post processing. The FarcadeGL renderer (`engine/gfx`, in progress) does.
