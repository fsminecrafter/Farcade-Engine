// fe.graphics (lua_graphics.h). The C functions below raise with luaL_error (longjmp), so they hold no
// objects with destructors.
#include "script/lua_graphics.h"
#include "gfx/graphics_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {
namespace {

using namespace fe::gfx;

// Choices that are numbers (shadow_resolution: 512, 1024 ...) read as numbers.
bool numeric_choice(const GraphicsProperty& p) { return p.kind == GraphicsPropKind::Choice && p.choices[0][0] >= '0' && p.choices[0][0] <= '9'; }

ScriptGraphics* binding_of(lua_State* L) {
    ScriptGraphics* b = (ScriptGraphics*)lua_touserdata(L, lua_upvalueindex(1));
    if (!b || !b->settings) luaL_error(L, "the graphics settings are not available here");
    return b;
}

void push_value(lua_State* L, const GraphicsSettings& s, int index) {
    const GraphicsProperty& p = graphics_property(index);
    char v[64];
    graphics_get_text(s, p.name, v, sizeof v);
    switch (p.kind) {
        case GraphicsPropKind::Bool: lua_pushboolean(L, strcmp(v, "true") == 0); break;
        case GraphicsPropKind::Integer:
        case GraphicsPropKind::Number: lua_pushnumber(L, (lua_Number)atof(v)); break;
        case GraphicsPropKind::Choice:
            if (numeric_choice(p)) lua_pushnumber(L, (lua_Number)atof(v)); else lua_pushstring(L, v);
            break;
    }
}

int g_get(lua_State* L) {
    ScriptGraphics* b = binding_of(L);
    const char* name = luaL_checkstring(L, 1);
    int i = graphics_property_find(name);
    if (i < 0) return luaL_error(L, "unknown graphics setting '%s'", name);
    push_value(L, *b->settings, i);
    return 1;
}

int g_set(lua_State* L) {
    ScriptGraphics* b = binding_of(L);
    const char* name = luaL_checkstring(L, 1);
    char text[64];
    switch (lua_type(L, 2)) {
        case LUA_TBOOLEAN: snprintf(text, sizeof text, "%s", lua_toboolean(L, 2) ? "true" : "false"); break;
        case LUA_TNUMBER: snprintf(text, sizeof text, "%.9g", (double)lua_tonumber(L, 2)); break;
        case LUA_TSTRING: snprintf(text, sizeof text, "%s", lua_tostring(L, 2)); break;
        default: return luaL_error(L, "graphics setting '%s': the value must be a string, number or boolean", name);
    }
    char err[200];
    if (!graphics_set_text(b->settings, name, text, err, sizeof err)) return luaL_error(L, "%s", err);
    if (b->changed) *b->changed = true;
    return 0;
}

int g_preset(lua_State* L) {
    ScriptGraphics* b = binding_of(L);
    const char* name = luaL_checkstring(L, 1);
    if (!graphics_apply_preset_name(b->settings, name))
        return luaL_error(L, "unknown graphics preset '%s' (allowed: low, medium, high, ultra)", name);
    if (b->changed) *b->changed = true;
    return 0;
}

int g_list(lua_State* L) {
    ScriptGraphics* b = binding_of(L);
    int n = graphics_property_count();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        const GraphicsProperty& p = graphics_property(i);
        lua_createtable(L, 0, 7);
        lua_pushstring(L, p.name);
        lua_setfield(L, -2, "name");
        const char* type = p.kind == GraphicsPropKind::Bool ? "boolean" : (p.kind == GraphicsPropKind::Integer ? "integer" :
                           (p.kind == GraphicsPropKind::Number ? "number" : "choice"));
        lua_pushstring(L, type);
        lua_setfield(L, -2, "type");
        push_value(L, *b->settings, i);
        lua_setfield(L, -2, "value");
        lua_pushstring(L, p.help);
        lua_setfield(L, -2, "help");
        if (p.kind == GraphicsPropKind::Choice) {
            lua_newtable(L);
            for (int k = 0; p.choices[k]; k++) {
                if (numeric_choice(p)) lua_pushnumber(L, (lua_Number)atof(p.choices[k])); else lua_pushstring(L, p.choices[k]);
                lua_rawseti(L, -2, k + 1);
            }
            lua_setfield(L, -2, "values");
        } else if (p.kind != GraphicsPropKind::Bool) {
            lua_pushnumber(L, (lua_Number)p.min);
            lua_setfield(L, -2, "min");
            lua_pushnumber(L, (lua_Number)p.max);
            lua_setfield(L, -2, "max");
        }
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int g_text(lua_State* L) {
    ScriptGraphics* b = binding_of(L);
    char buf[2048];
    graphics_settings_to_text(*b->settings, buf, sizeof buf);
    lua_pushstring(L, buf);
    return 1;
}

}  // namespace

void script_push_graphics_table(lua_State* L, ScriptGraphics* binding) {
    static const luaL_Reg funcs[] = { { "get", g_get }, { "set", g_set }, { "preset", g_preset }, { "list", g_list },
                                      { "text", g_text }, { nullptr, nullptr } };
    lua_newtable(L);
    lua_pushlightuserdata(L, binding);
    luaL_setfuncs(L, funcs, 1);
}

}  // namespace fe
