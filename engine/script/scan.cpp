// The one part of the script runtime that needs flecs: listing every entity
// (World can only list the children of an entity, not the roots).
#include "flecs.h"
#include "ecs/ecs.h"
#include "script/internal.h"

#include <string.h>

namespace fe {

static int depth_of(const World& w, Entity e) {
    int d = 0;
    while ((e = w.parent(e)) != 0) d++;
    return d;
}

Entity script_find_by_name(World& w, const char* name) {
    if (!name || !*name) return 0;
    if (Entity r = w.lookup(name)) return r;
    // Breadth first: the match nearest the roots wins (ties: first listed).
    Entity best = 0;
    int best_depth = 0x7fffffff;
    ecs_iter_t it = ecs_each_id(w.raw(), w.transform_component());
    while (ecs_each_next(&it)) {
        for (int i = 0; i < it.count; i++) {
            Entity e = it.entities[i];
            const char* n = w.name(e);
            if (!n || strcmp(n, name) != 0) continue;
            int d = depth_of(w, e);
            if (d < best_depth) { best_depth = d; best = e; }
        }
    }
    return best;
}

}  // namespace fe
