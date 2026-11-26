#!/usr/bin/env python3
"""Downloads the free assets of the example world (docs/example-world.md) into a cache folder.

  python3 tools/worldgen/fetch.py [--cache build/worldgen/cache] [--lite]

Standard library only. Files already in the cache (with the right checksum) are not downloaded again.

* Elevation: AWS Terrain Tiles (Terrarium PNG, public, no key; Mapzen / USGS / SRTM and other open data)
  covering the map around Åre, Sweden.
* Models and the ground texture: Poly Haven (CC0), glTF with 1k textures, plus each leaf material's separate
  alpha map (<part>_alpha), which tools/worldgen/build.py merges in with mofconv --alpha.
"""
import argparse
import hashlib
import json
import math
import os
import sys
import time
import urllib.request

API = "https://api.polyhaven.com"
TILES = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
UA = {"User-Agent": "farcade-worldgen/1.0"}

# The map: its centre, size (meters) and the elevation zoom. Zoom 10 is about 68 m per pixel at this latitude;
# fe-worldgen adds the relief between the samples. Above 60 N the finer zooms (11+) mix two elevation sources
# in patches that sit 40-90 m apart, which makes cliffs along the patch edges, so they are not used.
CENTER = (63.405, 13.080)
SIZE = {"full": 8192.0, "lite": 4096.0}
ZOOM = 10

# Poly Haven assets: id -> what the world uses it for. "big" ones (hundreds of MB) are left out with --lite.
ASSETS = {
    "fir_tree_01": {"role": "spruce", "big": True},
    "pine_tree_01": {"role": "pine", "big": True},
    "tree_small_02": {"role": "broadleaf"},
    "fir_sapling": {"role": "saplings"},
    "fir_sapling_medium": {"role": "saplings"},
    "pine_sapling_small": {"role": "saplings"},
    "shrub_02": {"role": "shrubs"},
    "shrub_03": {"role": "shrubs"},
    "shrub_04": {"role": "shrubs"},
    "fern_02": {"role": "shrubs"},
    "grass_medium_01": {"role": "grass"},
    "grass_medium_02": {"role": "grass"},
    "dandelion_01": {"role": "flowers"},
    "celandine_01": {"role": "flowers"},
    "boulder_01": {"role": "rocks"},
    "rock_moss_set_01": {"role": "rocks"},
    "stone_01": {"role": "rocks"},
    "tree_stump_01": {"role": "rocks"},
    "dead_tree_trunk": {"role": "rocks"},
}
GROUND = "forest_ground_04"     # Poly Haven texture: the terrain's detail texture


def get_json(url):
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                return json.load(r)
        except Exception as e:  # network hiccups: retry with backoff
            if attempt == 3:
                raise
            print(f"  retry {url}: {e}")
            time.sleep(2 ** attempt)


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, path, md5=None, size=None):
    if os.path.exists(path) and (md5 is None or md5_of(path) == md5) and (size is None or os.path.getsize(path) == size):
        return False
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".part"
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r, open(tmp, "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got = 0
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    got += len(b)
                    if total > 50 << 20:
                        print(f"\r  {os.path.basename(path)}: {got >> 20} / {total >> 20} MB", end="", flush=True)
                if total > 50 << 20:
                    print()
            if md5 and md5_of(tmp) != md5:
                raise IOError("checksum mismatch")
            os.replace(tmp, path)
            return True
        except Exception as e:
            if attempt == 3:
                raise
            print(f"  retry {url}: {e}")
            time.sleep(2 ** attempt)


def tile_range(size):
    lat, lon = CENTER
    n = 2 ** ZOOM
    def px(la, lo):
        x = (lo + 180) / 360 * n
        r = math.radians(la)
        y = (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n
        return x, y
    half = size / 2 + 300   # a margin for the bicubic filter
    dlat = half / 111320.0
    dlon = half / (111320.0 * math.cos(math.radians(lat)))
    x0, y0 = px(lat + dlat, lon - dlon)
    x1, y1 = px(lat - dlat, lon + dlon)
    return int(x0), int(y0), int(x1), int(y1)


def fetch_tiles(cache, size):
    x0, y0, x1, y1 = tile_range(size)
    d = os.path.join(cache, "tiles")
    count = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if download(TILES.format(z=ZOOM, x=x, y=y), os.path.join(d, f"{ZOOM}_{x}_{y}.png")):
                count += 1
    print(f"elevation: {(x1 - x0 + 1) * (y1 - y0 + 1)} tiles (zoom {ZOOM}, {count} new)")
    return d


def fetch_asset(cache, aid):
    files = get_json(f"{API}/files/{aid}")
    info = get_json(f"{API}/info/{aid}")
    g = files["gltf"]["1k"]["gltf"]
    d = os.path.join(cache, "assets", aid)
    gltf = os.path.join(d, os.path.basename(g["url"]))
    download(g["url"], gltf, g.get("md5"))
    for rel, f in g["include"].items():
        download(f["url"], os.path.join(d, rel), f.get("md5"))
    # Separate alpha maps: "<part>_alpha" (several materials) or "Alpha" (one). mofconv --alpha <part>=<file>.
    alphas = {}
    for key, res in files.items():
        if not (key.endswith("_alpha") or key == "Alpha") or not isinstance(res, dict) or "1k" not in res:
            continue
        png = res["1k"].get("png") or res["1k"].get("jpg")
        if not png:
            continue
        path = os.path.join(d, f"{key}_1k.png")
        download(png["url"], path, png.get("md5"))
        part = aid if key == "Alpha" else key[: -len("_alpha")]
        alphas[part] = path
    return {"gltf": gltf, "alphas": alphas, "name": info.get("name", aid), "authors": list(info.get("authors", {}).keys()),
            "url": f"https://polyhaven.com/a/{aid}", "license": "CC0"}


def fetch_ground(cache):
    files = get_json(f"{API}/files/{GROUND}")
    diff = files.get("Diffuse") or files.get("diff")
    j = diff["1k"]["jpg"]
    path = os.path.join(cache, "textures", f"{GROUND}_diff_1k.jpg")
    download(j["url"], path, j.get("md5"))
    info = get_json(f"{API}/info/{GROUND}")
    return {"path": path, "name": info.get("name", GROUND), "authors": list(info.get("authors", {}).keys()),
            "url": f"https://polyhaven.com/a/{GROUND}", "license": "CC0"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", default="build/worldgen/cache")
    ap.add_argument("--lite", action="store_true", help="a 4 km map; leaves out the big trees (about 1.4 GB)")
    a = ap.parse_args()
    size = SIZE["lite" if a.lite else "full"]
    manifest = {"center": CENTER, "size": size, "zoom": ZOOM, "tiles": fetch_tiles(a.cache, size), "assets": {}}
    for aid, meta in ASSETS.items():
        if a.lite and meta.get("big"):
            continue
        print(f"asset: {aid} ({meta['role']})")
        try:
            m = fetch_asset(a.cache, aid)
        except Exception as e:
            print(f"  skipped: {e}")
            continue
        m["role"] = meta["role"]
        manifest["assets"][aid] = m
    print(f"ground texture: {GROUND}")
    manifest["ground"] = fetch_ground(a.cache)
    path = os.path.join(a.cache, "manifest.json")
    with open(path, "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"wrote {path}")


if __name__ == "__main__":
    sys.exit(main())
