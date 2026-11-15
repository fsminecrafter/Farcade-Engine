// The one part of the audio system that needs flecs: listing the entities
// that have an audio module (World can only list the children of an entity).
#include "flecs.h"
#include "audio/internal.h"

#include <stdlib.h>

namespace fe {

int audio_scan(World& w, ModuleKind k, Entity** buf, int* cap) {
    int n = 0;
    ecs_iter_t it = ecs_each_id(w.raw(), w.component(k));
    while (ecs_each_next(&it)) {
        for (int i = 0; i < it.count; i++) {
            if (n == *cap) {
                int ncap = *cap ? *cap * 2 : 64;
                Entity* nb = (Entity*)realloc(*buf, sizeof(Entity) * (size_t)ncap);
                if (!nb) return n;
                *buf = nb;
                *cap = ncap;
            }
            (*buf)[n++] = it.entities[i];
        }
    }
    return n;
}

}  // namespace fe
