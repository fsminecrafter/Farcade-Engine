# Animation (the Animator module)

`.mof` has no animation data yet (the `ANIM` chunk is reserved, see
mof-format.md) and the renderer has no skinning, so the Animator plays
**transform clips** from a plain-text `.fanim` file: tracks that move, turn
and scale the animated object or its children.

```
fanim 1
clip spin 2.0                   # name and length in seconds (optional: the last key)
  track . rotation              # "." is the animated object, "arm/hand" a child path
    key 0   0 0 0               # time, then x y z (rotation in degrees)
    key 2   0 360 0
clip lift
  track lid position
    key 0 0 0 0
    key 1 0 0.5 0
```

- Values between keys are linear; rotation goes through quaternions (the short way).
- `track` channels: `position`, `rotation`, `scale`. Times must not go backwards.
- The file is `source` on the module, or the object's model path with `.fanim`.
- `loop` repeats a clip, otherwise it stops on its last key (`playing` becomes false).
- `speed` (negative plays backwards), `time` (set it to seek), `crossfade`.
- `animator:play(name, fade)` starts a clip, blending from the current one over
  `fade` seconds; `animator:stop()`. Setting `clip` or `playing` from Lua works too.
- A clip file with an error is logged once; the object stays as it is.

Not done: skeletal animation (needs bones in `.mof` and GPU skinning), editing clips in the editor.
