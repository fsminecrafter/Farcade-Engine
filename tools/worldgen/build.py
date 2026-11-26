#!/usr/bin/env python3
"""Builds the example world project (docs/example-world.md): fetch, convert, generate.

  python3 tools/worldgen/build.py [--out build/examples/are-valley] [--lite] [--offline] [--cache DIR]

1. tools/worldgen/fetch.py downloads the elevation tiles and the Poly Haven assets (unless --offline).
2. mofconv converts each model: alpha maps merged into the leaf materials, variants split into their own
   files, triangles reduced, LOD files written, textures downsized (budgets in ROLES below).
3. fe-worldgen writes the terrain, the foliage, the colour map, the scene and the project.

Then on Minimal-OS: farcade-editor.run --project <copy of out>, press Play (the camera flies a loop; B runs a benchmark lap).
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BIN = os.path.join(ROOT, "build", "host", "bin")

# Per role: the foliage layer it fills and how its models are converted.
#   tris: triangles of the full model, lods: of x_lod1 / x_lod2, tex: largest texture side,
#   variants: at most this many split variants per asset (each one is its own mesh in every 16 m foliage block,
#   so fewer variants mean fewer draws).
#   layer: density (instances per m2 at full paint), scale min / max, draw distance, LOD distance,
#   max slope, shadows, align to slope, shader
ROLES = {
    "grass":     dict(tris=160, lods=[], tex=512, variants=3,
                      layer=(1, 1.2, 1.6, 2.6, 70, 0, 35, 0, 1, "shaders/sway.fshader")),
    "flowers":   dict(tris=300, lods=[], tex=256, variants=2,
                      layer=(2, 0.3, 1.2, 1.8, 60, 0, 35, 0, 1, "shaders/sway.fshader")),
    "shrubs":    dict(tris=2000, lods=[400], tex=512, variants=2,
                      layer=(3, 0.12, 0.7, 1.3, 150, 50, 38, 0, 1, "shaders/sway.fshader")),
    "spruce":    dict(tris=12000, lods=[3000, 400], tex=1024, variants=3,
                      layer=(4, 0.014, 0.8, 1.4, 1000, 100, 35, 1, 0, "-")),
    "pine":      dict(tris=12000, lods=[3000, 400], tex=1024, variants=3,
                      layer=(5, 0.010, 0.8, 1.3, 1000, 100, 35, 1, 0, "-")),
    "broadleaf": dict(tris=8000, lods=[2000, 300], tex=1024, variants=3,
                      layer=(6, 0.008, 0.7, 1.2, 800, 80, 35, 1, 0, "-")),
    "saplings":  dict(tris=4000, lods=[1000, 200], tex=512, variants=3,
                      layer=(7, 0.02, 0.8, 1.6, 300, 50, 38, 1, 0, "-")),
    "rocks":     dict(tris=1500, lods=[300], tex=512, variants=3,
                      layer=(8, 0.02, 0.5, 2.0, 500, 60, 90, 1, 1, "-")),
}
# --lite: no big trees; the medium saplings stand in for them, scaled up.
LITE_STAND_IN = {"spruce": ("fir_sapling_medium", 3.0), "pine": ("pine_sapling_small", 4.0)}


def run(cmd):
    print("+", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit(f"failed: {cmd[0]}")
    return r.stdout


def convert(asset, meta, role, out, lite):
    """Converts one asset; returns the project relative paths of its (split) models."""
    p = ROLES[role]
    dst_dir = os.path.join(out, "models", asset)
    os.makedirs(dst_dir, exist_ok=True)
    tris = p["tris"] // (2 if lite else 1)
    cmd = [os.path.join(BIN, "mofconv"), meta["gltf"], os.path.join(dst_dir, asset + ".mof"), "--split",
           "--max-triangles", str(tris), "--max-texture", str(p["tex"] // (2 if lite and p["tex"] > 256 else 1))]
    if p["lods"]:
        cmd += ["--lods", ",".join(str(n // (2 if lite else 1)) for n in p["lods"])]
    for part, alpha in sorted(meta["alphas"].items()):
        cmd += ["--alpha", f"{part}={alpha}"]
    run(cmd)
    def variant(f):
        tail = os.path.basename(f)[len(asset) + 1:-4]
        return int(tail) if tail.isdigit() else 1 << 30
    files = sorted((f for f in glob.glob(os.path.join(dst_dir, asset + "_*.mof")) if "_lod" not in os.path.basename(f)), key=variant)
    if not files and os.path.exists(os.path.join(dst_dir, asset + ".mof")):
        files = [os.path.join(dst_dir, asset + ".mof")]
    files = files[: p["variants"]]
    return [os.path.relpath(f, out).replace(os.sep, "/") for f in files]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "examples", "are-valley"))
    ap.add_argument("--cache", default=os.path.join(ROOT, "build", "worldgen", "cache"))
    ap.add_argument("--lite", action="store_true")
    ap.add_argument("--offline", action="store_true", help="use the cache as it is")
    a = ap.parse_args()
    for tool in ("mofconv", "fe-worldgen"):
        if not os.path.exists(os.path.join(BIN, tool)):
            raise SystemExit(f"{tool} is not built: make host")
    if not a.offline:
        cmd = [sys.executable, os.path.join(HERE, "fetch.py"), "--cache", a.cache] + (["--lite"] if a.lite else [])
        subprocess.run(cmd, check=True)
    with open(os.path.join(a.cache, "manifest.json")) as f:
        man = json.load(f)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)

    # ---- models, by role
    models = {r: [] for r in ROLES}
    for aid, meta in man["assets"].items():
        role = meta["role"]
        if role not in ROLES:
            continue
        print(f"model: {aid} ({role})")
        models[role] += convert(aid, meta, role, out, a.lite)
    scale_boost = {}
    if a.lite:
        for role, (aid, k) in LITE_STAND_IN.items():
            if not models[role] and aid in man["assets"]:
                models[role] = [m for m in models["saplings"] if f"/{aid}/" in m]
                scale_boost[role] = k

    # ---- shaders and the layer table
    os.makedirs(os.path.join(out, "shaders"), exist_ok=True)
    shutil.copy(os.path.join(ROOT, "assets", "samples", "foliage", "sway.fshader"), os.path.join(out, "shaders", "sway.fshader"))
    lines = []
    for role, p in ROLES.items():
        if not models[role]:
            print(f"  (no models for {role}: layer left out)")
            continue
        idx, dens, smin, smax, dist, lod, slope, sh, al, shader = p["layer"]
        k = scale_boost.get(role, 1.0)
        if a.lite:
            dist = min(dist, 800)
        lines.append(f"layer {idx} {role} {role} {dens} {smin * k} {smax * k} {dist} {lod} {slope} {sh} {al} {shader} " + " ".join(models[role]))
    cfg = os.path.join(out, "layers.cfg")
    with open(cfg, "w") as f:
        f.write("\n".join(lines) + "\n")

    # ---- the world
    lat, lon = man["center"]
    size = man["size"]
    cell = 4 if a.lite else 2
    run([os.path.join(BIN, "fe-worldgen"), "--tiles", man["tiles"], "--out", out, "--layers", cfg, "--center", str(lat), str(lon),
         "--size", str(size), "--cell", str(cell), "--zoom", str(man["zoom"]), "--ground", man["ground"]["path"],
         "--name", "Are Valley" + (" Lite" if a.lite else "")])

    # ---- credits
    with open(os.path.join(out, "ASSETS.md"), "w") as f:
        f.write("# Assets of the Åre Valley example world\n\n")
        f.write("Built by tools/worldgen/build.py (Farcade Engine, docs/example-world.md).\n\n")
        f.write("## Elevation\n\nAWS Terrain Tiles (Terrarium), https://registry.opendata.aws/terrain-tiles/ : "
                "contains data from SRTM, USGS and other open sources (see the dataset's attribution).\n"
                f"Centre {lat}, {lon} (Åre, Sweden), {size:.0f} m a side, zoom {man['zoom']}.\n\n")
        f.write("## Models and textures (CC0, Poly Haven)\n\n| Asset | Use | Authors | Source |\n|---|---|---|---|\n")
        for aid, meta in man["assets"].items():
            f.write(f"| {meta['name']} | {meta['role']} | {', '.join(meta['authors'])} | {meta['url']} |\n")
        g = man["ground"]
        f.write(f"| {g['name']} | ground detail | {', '.join(g['authors'])} | {g['url']} |\n")
    print(f"done: {out}")


if __name__ == "__main__":
    sys.exit(main())
