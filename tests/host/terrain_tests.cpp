// Tests for the terrain heightfield (engine/scene/terrain.h): generate,
// sampling, brushes, undo / redo by revision, the file, ray hits, chunk and
// occluder meshes and the foliage scatter.
#include "scene/terrain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fe;

static int g_checks, g_failed;
static float absf_(float x) { return x < 0 ? -x : x; }
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t chunk_version(const TerrainData& t, int cx, int cz) { return t.chunk_info[cz * t.chunks_x + cx].version; }

int main() {
    // ---- generate and sample
    TerrainData t;
    CHECK(t.generate(4, 3, 16, 32.0f, 5.0f), "generate");
    CHECK(t.side_x() == 64 && t.side_z() == 48, "grid %d x %d", t.side_x(), t.side_z());
    CHECK(absf_(t.cell() - 2.0f) < 1e-6f, "cell %.3f", t.cell());
    bool inside = false;
    CHECK(absf_(t.height_at(40, 50, &inside) - 5.0f) < 1e-5f && inside, "flat height");
    t.height_at(-1, 3, &inside);
    CHECK(!inside, "outside is not inside");
    Vec3 n = t.normal_at(10, 10);
    CHECK(n.y > 0.999f, "flat normal up");
    int rev_gen = t.revision;
    CHECK(rev_gen > 0, "generate is a revision (%d)", rev_gen);

    // ---- raise brush, in a stroke
    uint32_t v_before = chunk_version(t, 1, 1), v_far = chunk_version(t, 3, 2);
    TerrainBrushOp op;
    op.brush = TerrainBrush::Raise;
    op.x = 48; op.z = 48; op.radius = 6; op.amount = 2.0f; op.hardness = 0.5f;
    t.begin_stroke();
    CHECK(t.apply(op), "raise changes");
    CHECK(t.end_stroke(), "stroke is a step");
    int rev_raise = t.revision;
    CHECK(rev_raise != rev_gen, "new revision");
    CHECK(absf_(t.height_at(48, 48) - 7.0f) < 1e-4f, "centre raised by amount (%.3f)", t.height_at(48, 48));
    CHECK(absf_(t.height_at(48, 60) - 5.0f) < 1e-4f, "outside the radius unchanged");
    CHECK(chunk_version(t, 1, 1) != v_before, "the chunk under the brush has a new version");
    CHECK(chunk_version(t, 3, 2) == v_far, "a far chunk keeps its version");
    Vec3 ns = t.normal_at(45, 48);
    CHECK(ns.x < -0.05f && ns.y > 0.3f, "slope normal leans away from the hill (%.2f %.2f)", ns.x, ns.y);

    // ---- level, smooth, lower
    op.brush = TerrainBrush::Level; op.level = 5.0f; op.rate = 1.0f; op.hardness = 1.0f;
    t.begin_stroke();
    t.apply(op);
    t.end_stroke();
    CHECK(absf_(t.height_at(48, 48) - 5.0f) < 1e-4f, "level flattens (%.3f)", t.height_at(48, 48));
    int rev_level = t.revision;
    op.brush = TerrainBrush::Lower; op.amount = 1.0f; op.hardness = 0.5f;
    t.begin_stroke(); t.apply(op); t.end_stroke();
    float dip = t.height_at(48, 48);
    CHECK(dip < 4.01f, "lower (%.3f)", dip);
    op.brush = TerrainBrush::Smooth; op.rate = 1.0f;
    t.begin_stroke();
    for (int i = 0; i < 20; i++) t.apply(op);
    t.end_stroke();
    CHECK(t.height_at(48, 48) > dip + 0.05f, "smooth fills the dip (%.3f > %.3f)", t.height_at(48, 48), dip);

    // ---- undo / redo through revisions
    CHECK(t.goto_revision(rev_raise), "undo to the raise");
    CHECK(absf_(t.height_at(48, 48) - 7.0f) < 1e-4f, "raise is back (%.3f)", t.height_at(48, 48));
    CHECK(t.goto_revision(rev_gen), "undo to generate");
    CHECK(absf_(t.height_at(48, 48) - 5.0f) < 1e-4f, "flat again");
    CHECK(t.goto_revision(rev_level), "redo to level");
    CHECK(absf_(t.height_at(48, 48) - 5.0f) < 1e-4f && t.revision == rev_level, "level state");
    CHECK(!t.goto_revision(123456), "unknown revision fails");
    // A new stroke after an undo drops the redo steps.
    t.goto_revision(rev_raise);
    op.brush = TerrainBrush::Raise; op.amount = 0.5f;
    t.begin_stroke(); t.apply(op); t.end_stroke();
    CHECK(!t.goto_revision(rev_level), "redo steps are gone after a new edit");
    CHECK(t.goto_revision(rev_raise), "but undo still works");

    // ---- holes
    op.brush = TerrainBrush::Delete; op.x = 10; op.z = 10; op.radius = 3;
    t.begin_stroke(); t.apply(op); t.end_stroke();
    t.height_at(10, 10, &inside);
    CHECK(!inside, "hole");
    float hit_t = 0;
    Ray down{ Vec3(10, 50, 10), Vec3(0, -1, 0) };
    CHECK(!t.raycast(down, 1000, &hit_t), "a ray falls through the hole");
    Ray down2{ Vec3(30, 50, 30), Vec3(0, -1, 0) };
    CHECK(t.raycast(down2, 1000, &hit_t) && absf_(hit_t - 45.0f) < 1e-3f, "ray hits the ground (%.3f)", hit_t);
    Ray slant{ Vec3(-20, 30, 20), normalize(Vec3(1, -0.5f, 0.1f)) };
    if (t.raycast(slant, 1000, &hit_t)) {
        Vec3 p = slant.origin + slant.dir * hit_t;
        CHECK(absf_(p.y - t.height_at(p.x, p.z)) < 1e-3f, "slanted hit lies on the surface");
    } else {
        CHECK(false, "slanted ray hits");
    }
    op.brush = TerrainBrush::Restore;
    t.begin_stroke(); t.apply(op); t.end_stroke();
    t.height_at(10, 10, &inside);
    CHECK(inside, "restored");

    // ---- chunk meshes
    uint32_t cap = TerrainData::mesh_capacity(t.cells);
    Vec3* pos = (Vec3*)malloc(sizeof(Vec3) * cap);
    Vec3* nrm = (Vec3*)malloc(sizeof(Vec3) * cap);
    float* uv = (float*)malloc(sizeof(float) * 2 * cap);
    uint32_t nv0 = t.build_chunk(0, 0, 0, 4.0f, pos, nrm, uv);
    CHECK(nv0 == cap, "full chunk with skirts fills the capacity (%u of %u)", nv0, cap);
    bool up = true, front = true;
    for (uint32_t i = 0; i < 16 * 16 * 6; i += 3) {
        up &= nrm[i].y > 0;
        Vec3 fn = cross(pos[i + 1] - pos[i], pos[i + 2] - pos[i]);
        front &= fn.y > 0;
    }
    CHECK(up && front, "top triangles face up");
    uint32_t nv1 = t.build_chunk(0, 0, 1, 4.0f, pos, nrm, uv);
    CHECK(nv1 == 8 * 8 * 6 + 4 * 8 * 6, "half detail (%u)", nv1);
    CHECK(t.max_lod() == 3, "lods for 16 cells (%d)", t.max_lod());

    // ---- occluder: never above the surface
    t.goto_revision(rev_raise);     // the hill
    Vec3* op_ = (Vec3*)malloc(sizeof(Vec3) * TerrainData::occluder_capacity());
    uint32_t no = t.build_occluder(1, 1, op_);
    CHECK(no > 0, "occluder triangles");
    bool below = true;
    for (uint32_t i = 0; i < no; i += 3)
        for (int k = 0; k < 7; k++) {
            float a = (float)(k % 3) / 3.0f, b = (float)(k / 3) / 4.0f;
            Vec3 p = op_[i] + (op_[i + 1] - op_[i]) * a + (op_[i + 2] - op_[i]) * (b * (1 - a));
            if (p.y > t.height_at(p.x, p.z) + 1e-4f) below = false;
        }
    CHECK(below, "occluder stays under the terrain");
    free(op_);

    // ---- foliage
    FoliageLayer fl;
    fl.density = 1.0f;
    op.brush = TerrainBrush::Paint; op.layer = 0; op.x = 20; op.z = 20; op.radius = 10; op.rate = 1.0f; op.target = 1.0f; op.hardness = 1.0f;
    t.begin_stroke(); CHECK(t.apply(op), "paint"); t.end_stroke();
    int bound = t.scatter_bound(0, 0, 0, fl);
    FoliageInstance* fi = (FoliageInstance*)malloc(sizeof(FoliageInstance) * (size_t)(bound + 1));
    int nf = t.scatter(0, 0, 0, fl, fi, bound);
    CHECK(nf > 150 && nf <= bound, "instances in the painted circle (%d, bound %d)", nf, bound);
    bool on_ground = true, in_circle = true, upright = true;
    for (int i = 0; i < nf; i++) {
        on_ground &= absf_(fi[i].pos.y - t.height_at(fi[i].pos.x, fi[i].pos.z)) < 1e-4f;
        float dx = fi[i].pos.x - 20, dz = fi[i].pos.z - 20;
        in_circle &= dx * dx + dz * dz < (10 + 3) * (10 + 3);
        upright &= fi[i].up.y > 0.999f;
    }
    CHECK(on_ground && in_circle && upright, "instances stand on the ground, upright, where painted");
    FoliageInstance* fi2 = (FoliageInstance*)malloc(sizeof(FoliageInstance) * (size_t)(bound + 1));
    int nf2 = t.scatter(0, 0, 0, fl, fi2, 50);
    CHECK(nf2 == 50 && memcmp(fi, fi2, sizeof(FoliageInstance) * 50) == 0, "the same instances, a prefix of the same order");
    op.brush = TerrainBrush::Erase;
    t.begin_stroke(); t.apply(op); t.end_stroke();
    CHECK(t.scatter(0, 0, 0, fl, fi, bound) == 0, "erased");
    free(fi); free(fi2);

    // ---- file round trip
    t.goto_revision(rev_raise);
    op.brush = TerrainBrush::Paint;
    t.begin_stroke(); t.apply(op); t.end_stroke();
    size_t sz = 0;
    uint8_t* blob = t.save(&sz);
    CHECK(blob && sz > 0, "save");
    TerrainData u;
    CHECK(u.load(blob, sz), "load");
    CHECK(u.chunks_x == 4 && u.chunks_z == 3 && u.cells == 16 && u.revision == t.revision, "header");
    CHECK(absf_(u.height_at(48, 48) - t.height_at(48, 48)) < 1e-6f, "heights");
    CHECK(u.density(0, 10, 10) == t.density(0, 10, 10) && u.density(0, 10, 10) > 0, "foliage");
    CHECK(!u.load(blob, sz / 2), "truncated file fails");
    printf("terrain file: %zu bytes for %d x %d cells\n", sz, t.side_x(), t.side_z());
    free(blob);

    free(pos); free(nrm); free(uv);
    printf("terrain_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
