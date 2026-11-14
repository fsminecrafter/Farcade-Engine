# Farcade Engine

A game engine for [Minimal-OS](https://github.com/fsminecrafter/Minimal-OS):
the editor and the games run on Minimal-OS; the engine itself is developed
on a PC (Linux) and tested there and in QEMU.

The plan behind it is `docs/farcade-engine-plan.md` in the Minimal-OS
repository.

## Layout

| Directory | What |
|---|---|
| `engine/` | the engine libraries (core, `.mof` models, ...) |
| `editor/` | the editor (Dear ImGui, drawn by a software rasterizer) |
| `runtime/minimalos/` | the small C/C++ runtime the engine needs on Minimal-OS |
| `tools/` | PC tools (model converter, CI scripts) |
| `tests/` | tests |
| `external/` | third-party code at pinned versions (`external/VERSIONS.md`) and the Minimal-OS SDK (submodule) |
| `mk/` | one make fragment per part |

## Building

```sh
git submodule update --init
make host      # Linux: tools, tests, headless editor
make test      # run the host tests
make os        # Minimal-OS programs (needs x86_64-elf-gcc/g++, see the SDK)
```

## Models: `.mof`

`.mof` is the engine's model file: meshes (positions, normals, tangents,
two UV sets, colors), materials, textures (raw RGBA8 with mips, PNG or
JPEG), a skeleton (bones with inverse bind matrices, joints and weights)
and a node tree, in one little-endian file of 16-byte aligned chunks. The
byte-level specification is `docs/mof-format.md`; the reader and writer
are `engine/mof/mof.h` (the reader is freestanding and zero-copy).

```sh
build/host/bin/mofconv model.glb model.mof     # from glTF 2.0 (.gltf/.glb) or OBJ (+MTL)
build/host/bin/mofconv --info model.mof        # what is inside
make mof-samples                               # rebuild assets/samples/*.mof
make test-mof-asan                             # the .mof tests under ASan/UBSan
```
