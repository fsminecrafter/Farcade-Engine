# Exporting a game

A game is a project (scenes, scripts, models, textures, sounds) plus the
prebuilt player. Exporting packs the project into one `.fepak`
(`docs/pak-format.md`) and puts a copy of the player next to it. Lua-only games
need nothing else: there is no compiler on the OS yet.

```
0:/games/MyGame/
  MyGame.run        the player (a copy of feplayer.run; on the PC: MyGame)
  MyGame.fepak      scenes, scripts, models, textures, audio, project.fe
  icon.bmp          the project's icon, when it has one (also inside MyGame.run)
```

## Project settings

`File > Project settings...` edits `project.fe` in the project folder:

```
farcade-project 1
name "My Game"
version "1.0"
entry_scene "scenes/main.fscene"
window_width 1280
window_height 720
icon "icon.bmp"
```

`graphics true` (the default) exports the graphics settings with the game:
saving the project settings or exporting writes the editor's current
Settings > Graphics to `graphics.cfg` in the project folder (text, one
`name value` per line, `editor/src/graphics_text.h`), the export packs it as
`graphics.cfg`, and the player applies it before the first frame (it prints
`FE PLAYER graphics settings from graphics.cfg`). Turn the box "Use these
graphics settings for the exported game" off to ship the defaults. The
parse/print functions in `graphics_text.cpp` are the single hook: when
`engine/gfx/graphics_settings` gets `to_text` / `from_text`, call them from
`graphics_settings_to_text` / `graphics_settings_from_text`.

`window_width` and `window_height` are the size of the window when MOSdesk
owns the screen (full screen uses the whole screen). The icon is a project
relative `.bmp`; the exporter writes it next to the game and bundles it into
the `.run` as `Resources/icon.bmp`, where MOSdesk looks for it. A file with no
`project.fe` works too: the folder's name is the game's name and the entry
scene is `scenes/main.fscene`.

## In the editor

`File > Export game...` asks for the game's name, the entry scene (the open
scene, if you press "Use open scene") and the output folder (default
`0:/games/<Name>/`). Export saves the scene if it has unsaved changes, writes
the settings to `project.fe`, then:

1. loads the entry scene and every other `.fscene` in the project (a few
   folders deep; `build/`, `export/` and dot folders are skipped),
2. collects the files they refer to: scripts and their file inputs, models,
   textures, shaders, and every path field of a module (audio files),
3. writes `<Name>.fepak` and copies the player to `<Name>.run`.

A referenced file that is missing is reported in the Console (and counted)
but does not stop the export. "Run the exported game" starts the result as
its own process.

## Command line

```sh
farcade-editor.run --export <project-or-scene> <outdir> [--name N] [--scene S]
                   [--player P] [--entry-only] [--run [FRAMES]]
build/host/bin/fe-export <project-or-scene> <outdir> ...      # the same on the PC, without the editor
```

The first argument is a project folder, or a `.fscene` file (its project is
the folder above `scenes/`, or its own folder; the scene becomes the entry
scene). `--entry-only` packs only the entry scene and what it uses. `--run`
starts the exported game afterwards (with `--frames FRAMES` when a number is
given). It prints `FE EXPORT OK <pak> files=N scenes=N bytes=N missing=N`, or
`FE EXPORT FAIL: ...` and exits 1. The editor needs no screen for this.

The player comes from `0:/programs/feplayer.run` on Minimal-OS (so install
`feplayer.run` next to the editor: `make os` builds both into `build/os/bin`)
and `build/host/bin/feplayer` on the PC; `--player` overrides it.

## Running a game

`MyGame.run` finds its package next to itself (`MyGame.fepak`, then
`game.fepak`, then `0:/games/MyGame/MyGame.fepak`), or takes it as its
argument. Esc or the window's close button quits.

```sh
run 0:/games/MyGame/MyGame.run                      # full screen, or a window under MOSdesk
run 0:/programs/feplayer.run 0:/games/MyGame/MyGame.fepak
feplayer game.fepak --frames 120 --png shot.png     # PC: headless, N frames, last frame as PNG
feplayer path/to/project --frames 60                # run loose project files (no export needed)
```

Options: `--scene <path>` starts at another scene, `--scale N` renders at 1/N
of the screen size and stretches it (slow CPUs), `--no-audio`, `--quiet`.
The player prints `FE PLAYER READY <name> (<w>x<h>, <objects> objects, <n>
scripts)` after the first frame and `FE PLAYER DONE frames=N checksum=XXXXXXXX
objects=N scripts=N` at the end (the checksum is of the last frame's pixels;
with `--frames` the time step is a fixed 1/60 s, so it is repeatable).
Lines that scripts `print` go to the same output.

The player runs the scene through the same `PlaySession`
(`editor/src/play_session.*`) as the editor's Play: Lua scripts, physics,
audio, the current camera, sky, sky light and post processing drawn by
FarcadeGL. Keyboard and mouse reach scripts as `fe.key_down("w")`,
`fe.key_pressed("space")`, `fe.mouse_x()`, `fe.mouse_down(0)` ... (see
`docs/scripting.md`); in the editor's Play the mouse is in viewport pixels.

## Notes

- Models (`.mof`) are packed and found through the VFS, but the engine does
  not draw them yet (a model object shows as a cube, in the editor too).
- A path in a scene that points outside the project cannot be packed; it is
  warned about.
- Packages are not compressed yet (the entry flags have room for zstd).
- The `.run` format (MINIRUN1) can carry files under `Resources/`; the
  exporter uses that for the icon only, because a running program has no
  documented way to read its own bundle. The package stays a separate file.
- Packaging as an installable `.mpkg` (installs to `0:/programs/<Game>.run` and
  `0:/games/<Game>/`) is not done yet.

## Testing

```sh
make host && make test     # tests/host/export_tests.cpp: pak, VFS, project.fe, export, and the headless player on an exported demo
MINIMAL_OS_DIR=... tools/ci/export_os_test.sh   # QEMU: the editor exports tests/data/export_demo, the exported player runs it
```
