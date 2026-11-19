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
| Character Controller 3D | `character_controller3d` | An upright capsule with gravity and collision response; scripts steer it with its live velocity and can read whether it is grounded |
| Audio Listener | `listener` | Where the game hears from. Has **Current** |
| Audio Stream | `audio_stream` | A long sound streamed from a file (music, ambience), optionally positioned in 3D |
| Audio Sampler | `audio_sampler` | Short sounds held in memory and played by scripts, several at once |
| Light | `light` | Directional, point or spot light |
| Particle Emitter | `particles` | CPU particles: rate, bursts, shapes (point, sphere, cone, box), lifetime, speed, gravity, drag, size/color/alpha over life, texture, additive or alpha blend, world or local space, loop or duration, prewarm, seed. Methods `play()`, `stop()`, `clear()`, `emit(n)` |
| Joint 3D | `joint3d` | Fixed, hinge (limits, motor), spring or ball-socket joint between this body and another body or the world; break force. Method `snap()` |
| Animator | `animator` | Plays clips of a `.fanim` file on the object and its children (docs/animation.md). Methods `play(name, fade)`, `stop()` |
| Force Field | `force_field` | A box or sphere that pushes bodies and particles inside it: directional, radial or vortex, with falloff and turbulence |
| Spawner | `spawner` | Makes copies of another object every so often (or by `spawn()`), with a random offset, velocity, lifetime and a limit. Methods `spawn()`, `clear()` |
| Audio Reverb Zone | `reverb_zone` | Echo and muffling for the listener inside a box or sphere |
| Billboard | `billboard` | A quad that always faces the camera (icons, foliage cards, glows) |
| Text 3D | `text3d` | A text label in the world, facing the camera |

Scripts steer a character controller by setting its world-space velocity in
meters per second. Its capsule collides with the existing physics bodies;
`grounded` reports contacts below the configured slope limit. For example:

```lua
local controller = self.object.character_controller3d
controller.velocity = fe.vec3(3, controller.grounded and 0 or controller.velocity.y, 0)
```

## Effects and gameplay modules

All parameters are fields: they are in the Inspector, in the `.fscene` file, and
in Lua as `obj.<module>.<field>` (`obj.particles.emission_rate = 80`,
`obj.joint3d.motor_speed = -90`, `obj.text3d.text = "Score " .. n`); methods are
called with a colon (`obj.particles:emit(20)`). Fields marked *live* are written
by the systems while playing (`particles.playing`, `particles.particle_count`,
`joint3d.angle`, `joint3d.force`, `joint3d.broken`, `animator.time`,
`spawner.alive` ...) and some are controls too (`particles.playing`,
`animator.clip`, `animator.time`). A link field (`joint3d.connected`,
`spawner.source`) is an object or nil in Lua.

- **Particle Emitter** (`engine/fx/particles.cpp`): a pool of at most 2000
  particles per emitter, one seeded random sequence per emitter, so the same
  seed gives the same particles. Cone and Box particles fly along the object's
  -Z, Sphere particles away from the center, Point particles anywhere. In the
  editor the selected emitters preview in the viewport and during Play all of
  them run; the game renderer draws them over the image (alpha or additive,
  hidden by meshes, `engine/gfx/particles.cpp`), the editor draws them with
  ImGui over the viewport.
- **Joint 3D** (`engine/physics`): rows of the sequential impulse solver. The
  object needs a Rigidbody 3D. The anchor is a point on this object that the
  joint keeps on the connected body (or in the world); a Spring hangs from the
  anchor to this object's origin. Hinge angle, speed and limits count this body
  turning about its axis. Connected bodies do not collide unless asked.
- **Force Field**: a box or sphere (its object's scale sizes it). Directional
  pushes along the field's direction, Radial away from (negative: towards) the
  center, Vortex around the object's Y axis. The force is in newtons on
  rigidbodies (mass 1 for particles); Linear and Quadratic falloff fade it to
  zero at the edge; Turbulence varies it in space and time. Layers select which
  collision layers of bodies it pushes.
- **Spawner**: copies the template object and its children into the running
  scene (the template stays: hide or park it). Copies appear at the spawner
  with the template's scale, get the added velocity (spawner's local space), a
  share of the spawner's own motion, and are destroyed after `lifetime` or when
  more than `max_alive` exist (the oldest first).
- **Animator**: see docs/animation.md. `.mof` has no animation data and the
  renderer has no skinning; clips animate transforms.
- **Audio Reverb Zone**: inside the zone (fading out over Fade distance
  outside) the whole mix gets a short reverb (Wet, Room size, Damping) and
  optionally a low-pass (Muffle). It is one effect on the master bus.
- **Billboard / Text 3D**: pictures that face the camera. Text uses a built-in
  5x7 bitmap font in the game renderer (ASCII). The editor shows a billboard
  as a tinted rectangle (textures show in the game).

Options to make them cheaper or turn them off: Settings > Graphics > Effects (graphics.cfg: particles, particle_density, particle_depth_test, sprites, audio_reverb; fe.graphics too)
(particles on/off, density, depth test, sprites, reverb), `FxQuality` in C++.

## Scene objects

These are object kinds with their own settings. You add them from
**Create** or the Hierarchy's + menu. They have no mesh: the viewport shows
them as an icon you can click, and cameras and lights also get their shape
drawn as lines.

| Object | Lua name | What it is for |
| --- | --- | --- |
| Camera | `camera` | Perspective or orthographic view along the object's −Z axis. Has **Current**, and up to three screen shaders (Shader 1-3, Shader params; `docs/graphics.md`) |
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

Scripts reach Current like any other field (see scripting.md):

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
- **Physics:** in Play, a built-in physics step runs Rigidbody 3D, Collider 3D and Character Controller 3D (`engine/physics`). It is a stand-in for Jolt (plan C6), which needs the standard C++ library this engine doesn't use yet. What it does:
  - Character Controller 3D uses an upright capsule (its default center puts the feet at the object's origin), gravity and the same collision solver. Set `character_controller3d.velocity` in m/s from a script to move it; `grounded` is updated by physics, using **Slope limit** to decide whether a contact counts as ground. A controller is not a Rigidbody: its rotation is fixed. A Collider 3D can provide friction, bounciness and layers; its shape and trigger mode are ignored.
  - Dynamic bodies fall and are damped (linear and angular) and turn as well as move: inertia comes from the collider shape, `freeze_rotation` locks the rotation, kinematic bodies move and turn by their velocities, and static bodies and colliders without a body don't move.
  - Box, sphere, capsule, cylinder and mesh colliders all collide with each other (GJK and EPA on convex shapes, contact manifolds of up to 4 points), so boxes rest flat and stack, spheres and cylinders roll down slopes, and boxes tip over edges. Friction, bounciness (no bounce below 1 m/s) and collision layers apply; the solver is sequential impulses with warm starting.
  - Mesh colliders use the convex hull of the points given to `PhysicsWorld::set_mesh` (the editor calls it with the object's mesh vertices when play starts); without points a mesh collider is its `size` box. Concave triangle meshes are not done.
  - Triggers report objects entering and leaving them, and first contacts are reported as hits. Scripts get them as `on_trigger_enter`, `on_trigger_exit` and `on_collision`, and the console logs them too.
  - `rigidbody3d.velocity` and `rigidbody3d.angular_velocity` are live.
  - Joints and Force Fields run in the same step (see Effects and gameplay modules).
  - Not done: continuous collision (very fast small bodies can tunnel through thin colliders), sleeping, concave meshes.
  - A new collider starts fitted to the object's mesh: a sphere for spheres, a thin box for planes.
- **Audio:** Audio settings are saved, but nothing plays them yet (plan C6: the engine's mixer over `SYS_AUDIO_STREAM`).
- **Rendering:** the editor's CPU preview does not use lights, sky, sky light or post processing. The FarcadeGL renderer (`engine/gfx`, in progress) does.
