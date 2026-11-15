# Vendored third-party code

Copied unmodified from upstream at these commits; each directory keeps its
licence file. To update one, copy the new files over and change this table.

| Directory | Upstream | Branch | Commit | Licence |
|---|---|---|---|---|
| imgui | https://github.com/ocornut/imgui | docking | cf53467 | MIT |
| imguizmo | https://github.com/CedricGuillemet/ImGuizmo | master | 105e4fd | MIT |
| stb | https://github.com/nothings/stb | master | 9600522 | MIT or public domain (stb_sprintf.h from deprecated/) |
| cgltf | https://github.com/jkuhlmann/cgltf | master | 85cd623 | MIT |
| fast_obj | https://github.com/thisistherk/fast_obj | master | d620667 | MIT |
| flecs | https://github.com/SanderMertens/flecs | tag v4.1.6 | fb55f3c | MIT (single-file distribution: flecs.c, flecs.h) |
| lua | https://www.lua.org/ftp/lua-5.4.7.tar.gz | release 5.4.7 | sha256 9fbf5e28 | MIT (src/*.c, *.h without lua.c, luac.c; the engine builds without liolib, loslib, loadlib, linit) |
| fgpu | https://github.com/fsminecrafter/Minimal-OS | - | a00577e | GPLv3 |
| Minimal-OS-SDK | https://github.com/fsminecrafter/Minimal-OS-SDK | main | git submodule | GPLv3 |

fgpu: the kernel's FGPU CPU backend, unmodified (no patches), laid out as the
kernel includes it (`#include "x86_64/..."`, build with `-Iexternal/fgpu`):
`fgpu_raster.c`, `fgpu_shader.c`, `fgpu_jit_x64.c`, `fgpu_ir.c` from
`src/modules/GPU/`, and `x86_64/fgpu_{raster,shader,jit_x64,ir,abi}.h` from
`src/intf/x86_64/`. The engine's host FGPU device (engine/gfx/host) drives them
so the SDK's farcadegl.h runs on the PC; `fgpu_jit_x64.c` and `fgpu_shader.c`
are built with `-DFGPU_ENABLE_JIT` (mk/gfx.mk), as the kernel always has the JIT.

Fonts in editor/fonts come from MOSdesk (Inter: SIL OFL 1.1, Roboto Mono:
Apache 2.0); their licence files are next to them.
