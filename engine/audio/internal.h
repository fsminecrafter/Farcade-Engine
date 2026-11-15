// Internal to engine/audio.
#pragma once
#include "audio/audio.h"

namespace fe {

// Writes the entities that have module `k` into *buf (realloc'd, *cap its
// capacity in entries). Returns the count. scan.cpp (needs flecs).
int audio_scan(World& w, ModuleKind k, Entity** buf, int* cap);

}  // namespace fe
