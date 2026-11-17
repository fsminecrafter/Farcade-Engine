// fe.graphics: scripts read and change the graphics settings at run time.
//
//   fe.graphics.get("aa")              -> "fxaa"       (a string for choices, a number for numbers and for
//                                                       numeric choices such as shadow_resolution, or a boolean)
//   fe.graphics.set("aa", "off")                         (a string, number or boolean; raises a Lua error
//   fe.graphics.set("shadow_resolution", 2048)            for an unknown name or an invalid value)
//   fe.graphics.preset("low")                            (low | medium | high | ultra)
//   fe.graphics.list()                 -> { { name, type = "choice"|"boolean"|"integer"|"number",
//                                              value, values = { ... } (choices), min, max, help }, ... }
//   fe.graphics.text()                 -> the settings as text (engine/gfx/graphics_settings.h)
//
// The names and allowed values are the ones of graphics_settings.h. A change writes to the host's
// GraphicsSettings and sets its `changed` flag; the host passes the settings to the renderer
// (Renderer::set_settings) before the next frame.
//
// Hook: ScriptRuntime::set_graphics_settings(&settings, &changed) (runtime.h) gives the runtime the
// host's settings; runtime.cpp registers the table with script_push_graphics_table in init().
#pragma once

#include "script/lua_api.h"

namespace fe::gfx { struct GraphicsSettings; }

namespace fe {

struct ScriptGraphics {
    gfx::GraphicsSettings* settings = nullptr;      // null: scripts get an error (no renderer)
    bool* changed = nullptr;
};

// Pushes the fe.graphics table; its functions read `binding` when called (it must outlive the Lua state).
void script_push_graphics_table(lua_State* L, ScriptGraphics* binding);

}  // namespace fe
