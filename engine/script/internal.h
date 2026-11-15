// Between the script runtime's files; not for users of runtime.h.
#pragma once
#include "ecs/ecs.h"

namespace fe {

// fe.find: the root entity called `name`, else the nearest match in the
// whole tree (0 if none). Implemented in scan.cpp (it needs flecs).
Entity script_find_by_name(World& w, const char* name);

}  // namespace fe
