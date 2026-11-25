// Terrain objects in the editor, Play and the player: their data (the
// .fterrain files, loaded through the VFS and saved with the scene), chunk
// streaming, chunk meshes at distance-based detail, foliage batches and
// which objects are loaded (the Object's Streaming property).
//
// Data store: one TerrainData per file path, shared by every object that
// names it. terrain_data() loads it the first time and moves it to the
// revision the object's settings carry, so the editor's undo / redo (scene
// snapshots holding Terrain::revision) also undoes brush strokes.
//
// TerrainView (one per renderer) is told the camera every frame:
// - chunks: loaded when within the terrain's load_distance of the camera
//   (always in the editor unless the terrain's stream_in_editor is on). A
//   loaded chunk is drawn at full detail up to lod_distance, then at half,
//   a quarter and an eighth of its cells per further step, with skirts over
//   the cracks; each comes with a coarse occluder mesh for occlusion culling;
// - Dynamic objects standing on an unloaded chunk are not drawn; Unloaded
//   objects wait for a script (obj.loaded = true) while playing;
// - foliage: each loaded chunk and layer gets one batch per model part: the
//   instances' triangles merged into one mesh in random priority order. The
//   renderer draws a prefix of it (RenderMesh::draw_count) that shrinks with
//   distance, so millions of grass blades cost a few draws per chunk, far
//   chunks draw few of them and chunks past the layer's distance none.
//   Batches are built a few per frame (pending() says more are waiting) and
//   dropped least recently used over a memory budget.
#pragma once

#include "scene.h"
#include "scene/terrain.h"
#include "gfx/renderer.h"
#include <stddef.h>
#include <stdint.h>

namespace fe {

// ---------------------------------------------------------------- data store

// The data of a Terrain object, or null (no file yet, or it cannot be read).
TerrainData* terrain_data(Object& o);
// New empty data under `path` (replacing what the store had for it); the object should name it.
TerrainData* terrain_new_data(const char* path);
// A copy of the data at `from` stored under `to` (duplicating a terrain object). False when there is none.
bool terrain_copy_data(const char* from, const char* to);
// Edited: saved on the next terrain_store_save, and the views redraw.
void terrain_mark_dirty(TerrainData* t);
bool terrain_store_unsaved();
// Writes every changed data file that a Terrain object of `sc` names through `write` (path as stored,
// bytes); false when one failed. Data of terrains no longer in the scene (deleted, undone) is not written.
bool terrain_store_save(Scene& sc, bool (*write)(const char* path, const void* data, size_t size, void* user), void* user);
// Forgets every terrain (a scene was opened or a new one started): they load from their files again.
void terrain_store_clear();
// Counts every change to any terrain's data (brush applications too): part of the editor's view hash.
uint64_t terrain_store_change();
void terrain_store_touch();
// The path a new terrain of `o` gets: terrain/<name>_<id>.fterrain.
void terrain_default_path(const Object& o, char* out, size_t cap);
// The world position of a terrain's first corner (its object's world translation).
Vec3 terrain_origin(Scene& sc, const Object& o);

// ---------------------------------------------------------------- view

struct TerrainViewStats {
    int terrains = 0;
    int chunks = 0, chunks_drawn = 0, chunks_unloaded = 0;
    int lod_chunks[kTerrainLods] = {};
    int objects_unloaded = 0;
    int foliage_batches = 0;                // drawn this frame
    long foliage_instances = 0;             // drawn this frame
    int foliage_built = 0;                  // batches (re)built this frame
    bool pending = false;                   // batches still waiting to be built
    size_t memory = 0;                      // meshes and batches held, bytes
};

class TerrainView {
public:
    TerrainView() = default;
    ~TerrainView();
    TerrainView(const TerrainView&) = delete;
    TerrainView& operator=(const TerrainView&) = delete;

    // Starts a frame from the camera: chunk streaming, meshes, foliage.
    // `playing`: streaming applies to every terrain (else only those with stream_in_editor).
    void update(Scene& sc, const Vec3& eye, bool playing);
    // Whether an object is loaded this frame (call after update()).
    bool loaded(Scene& sc, const Object& o) const;
    // The draw items of the terrains (chunks, then foliage), valid until the next update().
    const gfx::DrawItem* items() const { return items_; }
    int item_count() const { return item_count_; }
    // The terrain object each item belongs to (picking ids in the CPU view).
    const ObjectId* item_objects() const { return item_obj_; }
    const TerrainViewStats& stats() const { return stats_; }
    bool pending() const { return stats_.pending; }
    void clear();                           // drops every cached mesh and batch
    // Foliage build time per frame (ms); the rest waits for the next frames.
    double build_budget_ms = 6.0;
    size_t memory_budget = (size_t)384 << 20;

    struct Cache;
private:
    Cache* caches_ = nullptr;
    int cache_count_ = 0, cache_cap_ = 0;
    gfx::DrawItem* items_ = nullptr;
    ObjectId* item_obj_ = nullptr;
    int item_count_ = 0, item_cap_ = 0;
    uint64_t frame_ = 0;
    bool playing_ = false;
    TerrainViewStats stats_;
    gfx::DrawItem* push(ObjectId id);
    void evict(size_t budget);
};

}  // namespace fe
