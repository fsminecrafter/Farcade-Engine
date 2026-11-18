// Shared by the fx systems: listing the entities that have a module (World
// can only list the children of an entity). Needs flecs, so it lives in
// scan.cpp.
#pragma once

#include "ecs/ecs.h"

namespace fe {

// Writes the entities that have module k into *buf (grown with realloc,
// capacity in *cap) and returns how many.
int fx_scan(World& w, ModuleKind k, Entity** buf, int* cap);

}  // namespace fe
