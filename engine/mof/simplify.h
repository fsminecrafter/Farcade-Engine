// Triangle reduction of an imported model (mof_wdesc), for LODs and for importing very detailed assets.
// Built on meshoptimizer's simplifier (external/meshoptimizer, MIT).
//
// Every submesh is reduced to about `ratio` of its triangles: first with the attribute preserving
// simplifier (it keeps UV seams and the outline), then, if that cannot get close enough (piles of small
// separate pieces such as leaf cards or needles), with the "sloppy" one that merges nearby geometry. The
// vertices each mesh still uses are compacted, so a reduced file is small too. Materials, textures, nodes
// and bones are kept as they are (shared with the input: it must outlive the result).
#pragma once
#include "mof/mof.h"

struct mof_simplified {
    mof_wdesc desc;                 // ready for mof_write; points into the input and into `blocks`
    void** blocks;
    size_t block_count, block_cap;
    uint64_t triangles_before, triangles_after;
};

// Triangles a model draws (meshes counted once, as stored).
uint64_t mof_desc_triangles(const mof_wdesc* d);
// ratio in (0, 1]. False (with a message) on out of memory.
bool mof_simplify(const mof_wdesc* in, float ratio, mof_simplified* out, char* err, size_t errlen);
void mof_simplified_free(mof_simplified* s);
