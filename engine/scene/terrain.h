// Terrain: a heightfield split into square chunks (the Terrain object of
// scene/modules.h), its editing brushes, chunk meshes, foliage scatter and
// the .fterrain file. Plain C++ with no renderer types: the editor, the play
// session, the player and the physics all use it.
//
// Layout: chunks_x * chunks_z chunks of `cells` x `cells` grid cells,
// chunk_size meters a side. Heights sit on the grid's vertices
// ((side_x + 1) x (side_z + 1), row by row along x, rows along z), holes and
// the painted foliage density (one byte per foliage layer) on its cells.
// Coordinates are local to the terrain: x and z from 0 at the first corner,
// y relative to the terrain object's position.
//
// Chunks have versions: an edit bumps the version of every chunk whose mesh
// (heights, normals, holes) or foliage changed, so callers rebuild only those.
//
// Undo: brush strokes (begin_stroke .. end_stroke) and Generate are history
// steps; each gets a new revision number. The editor stores the revision in
// the Terrain settings (Terrain::revision), so its scene snapshots carry it,
// and goto_revision() moves the data to the revision an undo or redo brought
// back. A step remembers only the chunks the stroke touched.
//
// Foliage: instances are not stored. scatter() places them again from the
// painted density with a fixed hash (the same density gives the same grass),
// in a random priority order, so drawing the first N of a chunk's instances
// is an even thinning of all of them (distant chunks draw fewer).
#pragma once

#include "core/math.h"
#include "scene/modules.h"
#include <stddef.h>
#include <stdint.h>

namespace fe {

constexpr int kTerrainMaxCells = 4096;      // grid cells per side at most (chunks * cells)
constexpr int kTerrainLods = 4;             // chunk mesh detail levels: 1, 1/2, 1/4, 1/8 of the cells

enum class TerrainBrush : uint8_t {
    Raise,      // Add: up by `amount` meters at the centre
    Lower,      // Remove: down by `amount`
    Smooth,     // towards the average of the neighbours (rate)
    Level,      // towards `level` (rate)
    Delete,     // cuts holes (cells inside the radius)
    Restore,    // fills holes back in
    Paint,      // foliage layer `layer` towards `target` (rate)
    Erase,      // foliage layer `layer` towards 0 (rate)
    Count
};

struct TerrainBrushOp {
    TerrainBrush brush = TerrainBrush::Raise;
    float x = 0, z = 0;             // centre, terrain local
    float radius = 4.0f;            // meters
    float hardness = 0.5f;          // 0: soft edge all the way in, 1: hard edge
    float amount = 0.1f;            // Raise / Lower: meters at the centre for this application
    float rate = 0.5f;              // Smooth / Level / Paint / Erase: 0..1 of the way per application
    float level = 0.0f;             // Level: the height (terrain local)
    int layer = 0;                  // Paint / Erase
    float target = 1.0f;            // Paint: density 0..1
};

struct TerrainChunkInfo {
    uint32_t version = 1;           // heights or holes (the mesh) changed
    uint32_t foliage_version = 1;   // painted foliage (or the heights under it) changed
    float hmin = 0, hmax = 0;       // height range (valid while bounds_version == version)
    uint32_t bounds_version = 0;
};

// A foliage instance (terrain local): stands at pos, turned by yaw around `up`.
struct FoliageInstance {
    Vec3 pos;
    Vec3 up;
    float yaw = 0;
    float scale = 1;
};

class TerrainData {
public:
    TerrainData() = default;
    ~TerrainData();
    TerrainData(const TerrainData&) = delete;
    TerrainData& operator=(const TerrainData&) = delete;

    // A flat terrain at `height` (a history step when there was data before).
    // Sizes are clamped: cells 4..128 per chunk, at most kTerrainMaxCells
    // cells a side. False when out of memory.
    bool generate(int chunks_x, int chunks_z, int cells, float chunk_size, float height);
    bool valid() const { return heights != nullptr; }
    void clear();                   // no data, no history

    int chunks_x = 0, chunks_z = 0;
    int cells = 0;                  // per chunk side
    float chunk_size = 0;           // meters
    int side_x() const { return chunks_x * cells; }
    int side_z() const { return chunks_z * cells; }
    float cell() const { return cells ? chunk_size / (float)cells : 1.0f; }
    float size_x() const { return chunk_size * (float)chunks_x; }
    float size_z() const { return chunk_size * (float)chunks_z; }
    int chunk_count() const { return chunks_x * chunks_z; }

    float* heights = nullptr;
    uint8_t* holes = nullptr;                       // per cell, 1 = hole
    uint8_t* foliage[kFoliageLayers] = {};          // per cell density 0..255; null = nothing painted
    TerrainChunkInfo* chunk_info = nullptr;
    int revision = 0;
    uint64_t serial = 0;            // set by whoever owns it, to tell data sets apart (mesh cache keys)

    // Grid access (clamped to the edges).
    float h(int ix, int iz) const;
    bool hole(int cx, int cz) const;                // outside = hole
    uint8_t density(int layer, int cx, int cz) const;

    // The surface at (x, z): the height of the triangle the mesh draws there.
    // *inside (optional) is false outside the terrain or over a hole.
    float height_at(float x, float z, bool* inside = nullptr) const;
    Vec3 normal_at(float x, float z) const;
    // First hit of a ray (terrain local), holes let it through. False when none within max_t.
    bool raycast(const Ray& r, float max_t, float* t) const;

    // ---- editing
    // Changes the data under the brush; true when something changed.
    // Outside a stroke the change is not a history step.
    bool apply(const TerrainBrushOp& op);
    void begin_stroke();
    // Ends the stroke: when it changed something, a history step with a new
    // revision (returned true; `revision` is the new one).
    bool end_stroke();
    bool in_stroke() const;
    // Undo / redo to the data of revision r. False when no history step leads there.
    bool goto_revision(int r);
    int history_steps() const;      // undo steps kept (tests)

    // ---- chunks
    // Height range of a chunk (local y), cached per version.
    void chunk_bounds(int cx, int cz, Vec3* lo, Vec3* hi);
    // Mesh of chunk (cx, cz) at detail `lod` (0 = every cell; each level
    // halves): a triangle list with skirts down its edges (they hide the
    // cracks between chunks of different detail). Writes positions, normals
    // and uvs (2 floats, uv_scale meters per repeat) and returns the vertex
    // count, at most mesh_capacity(cells). Holes are left out.
    static uint32_t mesh_capacity(int cells);
    uint32_t build_chunk(int cx, int cz, int lod, float uv_scale, Vec3* pos, Vec3* nrm, float* uv) const;
    int max_lod() const;            // the coarsest level this cell count has
    // A coarse mesh for occlusion culling that never lies above the real
    // surface (every vertex takes the lowest height around it); no skirts,
    // no blocks with holes. At most occluder_capacity() vertices.
    static uint32_t occluder_capacity();
    uint32_t build_occluder(int cx, int cz, Vec3* pos) const;

    // ---- foliage
    // The instances of a layer in a chunk, highest priority first (at most cap).
    int scatter(int cx, int cz, int layer, const FoliageLayer& fl, FoliageInstance* out, int cap) const;
    // An upper bound of scatter()'s count for this layer and chunk.
    int scatter_bound(int cx, int cz, int layer, const FoliageLayer& fl) const;
    bool has_foliage(int cx, int cz, int layer) const;

    // ---- file (.fterrain)
    uint8_t* save(size_t* size) const;              // malloc'd
    bool load(const void* data, size_t size);       // keeps the history of the editor session out: clears it
    size_t memory() const;                          // bytes of data and history

    struct History;
private:
    bool alloc(int cx, int cz, int cells_, float size);
    void free_data();
    void bump(int ix0, int iz0, int ix1, int iz1, bool mesh, bool foliage_too);   // vertex range
    void snapshot(int ix0, int iz0, int ix1, int iz1);                           // before a change
    void touch_cells(int x0, int z0, int x1, int z1, bool mesh);                 // cell range changed
    bool ensure_layer(int layer);
    History* hist_ = nullptr;
};

}  // namespace fe
