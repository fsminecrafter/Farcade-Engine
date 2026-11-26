# The example world: Åre Valley

A big example project for stress-testing the engine. It is a real place: the
valley around Åre, Sweden (63.4° N, 13.1° E), built from public elevation
data. Forested hills run along the lake Åresjön and rise to bare alpine
slopes. Free CC0 models from Poly Haven cover it: spruce, pine, small
broadleaf trees, saplings, shrubs, ferns, grass, flowers, rocks and dead wood. Grass,
flowers and shrubs sway in the wind, and two layers of volumetric clouds
drift over it.

Only the tools and an asset list are in the repository. A script downloads
the assets and builds the project on your machine.

## Build it

```bash
make example-world
```

This writes the full world to `build/examples/are-valley`: 8 × 8 km, 2 m
cells, the big trees. It downloads about 1.5 GB the first time.

```bash
make example-world-lite
```

This writes a smaller world to `build/examples/are-valley-lite`: 4 × 4 km,
4 m cells, no big trees (saplings scaled up stand in for them), lighter
models and shorter draw distances. It downloads about 100 MB. Use it on
Minimal-OS.

The downloads are cached in `build/worldgen/cache`, so a second build is
offline:

```bash
python3 tools/worldgen/build.py --lite --offline --out build/examples/are-valley-lite
```

## Open it

**In the editor (Minimal-OS).** Copy the project folder to the disk, for
example to `0:/projects/are-valley-lite`, then run:

```
run 0:/programs/farcade-editor.run --project 0:/projects/are-valley-lite
```

Press **Play**. The Flythrough Camera flies a loop over the valley, about
two minutes a lap, and the status line shows the frame time.

| Key | What it does |
| --- | --- |
| Space | Pause and resume the flight |
| B | A benchmark lap: when it ends, the console shows the frame time's minimum, average and maximum |

**As a game.** Export it like any project (`docs/export.md`), then run the
`.run` with its `.fepak`. The export also packs each foliage model's
`_lod1` and `_lod2` files.

```bash
build/host/bin/fe-export build/examples/are-valley-lite out/
```

**On the PC, without a screen.** The headless player runs the project folder
directly and writes the last frame:

```bash
build/host/bin/feplayer build/examples/are-valley-lite --frames 60 --png shot.png
```

For a still image without a screen:

```bash
build/host/bin/fe-editor-shot --world build/examples/are-valley-lite shot.png -400 +3 470 150 6 30
```

The arguments are the orbit target x, y, z, then yaw, pitch and distance. A
y of `+N` means N metres above the ground. Optional extra arguments are the
frame count and the preset (`low`, `high` or `ultra`). The tool builds all
the foliage before the shot and prints the chunk, batch, instance and
triangle counts.

## What is in it

| Object | What it is |
| --- | --- |
| Terrain | The heightfield in chunks of about 256 m. Its texture is a colour map stretched over the whole map (forest, meadow, heath, rock, snow, lake bed), multiplied by a tiling forest-floor detail texture up close. It has 8 foliage layers |
| Lake Aresjon | A flat plane at the lake's level, with a smooth metallic material (screen-space reflections) |
| Sun, Sky, Sky Light | Low northern sun, gradient sky with a light valley haze, probe GI |
| Clouds | Volumetric clouds, Ultra quality, 2 layers about 700 m above the summit, with wind |
| Post Processing | AgX tone mapping, auto exposure, bloom |
| Flythrough Camera | The camera and `scripts/flythrough.lua` (a Catmull-Rom loop) |

The foliage layers. Distances are for the full world; the lite world caps
them at 800 m.

| Layer | Models | Density / m² | Distance | LOD at | Shadows | Sway |
| --- | --- | --- | --- | --- | --- | --- |
| 1 grass | grass_medium_01, 02 | 1.2 | 70 m | | | yes |
| 2 flowers | dandelion, celandine | 0.3 | 60 m | | | yes |
| 3 shrubs | shrub_02, 03, 04, fern_02 | 0.12 | 150 m | 50 m | | yes |
| 4 spruce | fir_tree_01 | 0.014 | 1000 m | 100 m | yes | |
| 5 pine | pine_tree_01 | 0.010 | 1000 m | 100 m | yes | |
| 6 broadleaf | tree_small_02 | 0.008 | 800 m | 80 m | yes | |
| 7 saplings | fir_sapling, fir_sapling_medium, pine_sapling_small | 0.02 | 300 m | 50 m | yes | |
| 8 rocks | boulder, moss rocks, stone, stump, dead trunk | 0.02 | 500 m | 60 m | yes | |

Where each layer grows comes from the land itself. The tree line is at about
820 m (± 70 m with noise). Forest grows in stands with clearings. There are
meadows near the lake and along the valley floor, heath and rock above the
tree line, and nothing on steep slopes or in the lake.

## How it is made

`tools/worldgen/build.py` runs three steps:

1. **`tools/worldgen/fetch.py`** downloads, using the Python standard library only:
   - The elevation tiles: AWS Terrain Tiles, Terrarium PNG, zoom 10 (about
     68 m a pixel here).
   - The Poly Haven models as glTF with 1k textures, each leaf material's
     separate alpha map, and the ground texture.

   Every file is checked against its md5. The step writes
   `build/worldgen/cache/manifest.json`.
2. **`mofconv`** converts each model (`docs/mof-format.md`, section 10):
   - `--alpha part=file` merges the alpha map into the leaf material (alpha cut-out, two-sided).
   - `--split` gives one file per variant (Poly Haven lays variants side by side).
   - `--max-triangles` reduces each one.
   - `--lods` writes `_lod1` and `_lod2`.
   - `--max-texture` downsizes the textures.

   The budgets are in the `ROLES` table of `build.py`.
3. **`fe-worldgen`** (`tools/worldgen/main.cpp`) writes the project:
   - The heights: the tiles, bicubic, plus fractal relief between the samples (ridged in the alpine part).
   - The lake: flat at its level, with the bed carved under it.
   - The foliage density of each layer per cell.
   - The colour map and the detail texture.
   - The scene, the flythrough script, `project.fe`, and `graphics.cfg` (Ultra).

Why zoom 10: above 60° N the finer zoom levels mix two elevation sources in
patches that sit 40 to 90 m apart. That makes cliffs along the patch edges.
Zoom 10 is clean. The fractal relief adds the detail between its samples.

`fe-worldgen` also runs on its own, with any Terrarium tiles and a layer table:

```bash
build/host/bin/fe-worldgen --tiles DIR --out PROJECT --layers layers.cfg [--center LAT LON] [--size M] [--cell M] [--zoom Z] [--ground IMG] [--name NAME]
```

Each line of the layer table has this form:

```
layer <1-8> <kind> <name> <density> <scale min> <scale max> <distance> <lod distance> <max slope> <shadows 0/1> <align 0/1> <shader or -> <model> <model> ...
```

`<kind>` is one of `grass`, `flowers`, `shrubs`, `spruce`, `pine`,
`broadleaf`, `saplings` or `rocks`. It chooses where the layer grows.

`worldgen_tests` (part of `make test`) runs it on made-up tiles, without the
network.

## What it stresses, and what it found

- **Many meshes in one frame.** A whole loaded map plus its foliage blocks
  can come to thousands of meshes. The renderer's mesh cache holds 2048
  buffers, and GPU memory can run out. The first version of the world made
  FarcadeGL drop the whole frame back to the CPU preview. Now:
  - The meshes that don't fit are left out of that frame and counted
    (`RendererStats::meshes_dropped`). The rest draws.
  - The world uses big chunks (one mesh each).
  - Each layer uses at most a few model variants (each variant is a mesh in
    every 16 m foliage block).
- **Triangles.** Foliage is baked into vertex buffers per block, because
  FarcadeGL has no instancing. The budgets above keep a typical view at
  about 1 to 2 million triangles in the lite world. Densities from the first
  version (one tree per 28 m²) came to 12 million triangles and 1.7 GB of
  batches.
- **Far forests.** Trees end at their draw distance. Beyond it the colour map
  paints forest canopy dark green, so a forest still reads as one from
  afar. Impostors (billboards) for far trees would be the next engine step.
- **Fog.** The sky's `fog` is a density of 0.06 × fog per metre. The world
  uses 0.0015 (about half visibility at 8 km).
- **The editor's far plane.** The view now reaches as far as a terrain loads
  (`load_distance`).

## Sources and licences

Each built project has an `ASSETS.md` that lists every asset with its authors
and link.

- **Elevation:** AWS Terrain Tiles (https://registry.opendata.aws/terrain-tiles/).
  It contains data from SRTM, GMTED2010, ETOPO1 and other open sources; see
  the dataset's attribution.
- **Models and the ground texture:** Poly Haven (https://polyhaven.com), CC0.
