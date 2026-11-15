// Lua's C API for the engine's C++: one place for the extern "C" wrap.
#pragma once
#include <limits.h>
// GCC's limits.h hides the long long limits in strict C++ modes, and
// luaconf.h takes their absence for a compiler without long long.
#ifndef LLONG_MAX
#define LLONG_MAX __LONG_LONG_MAX__
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif
extern "C" {
#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"
}
