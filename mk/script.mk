# The Lua script runtime (engine/script): Lua 5.4.7 (external/lua, vendored
# unmodified) plus fe::ScriptRuntime. See docs/scripting.md.
#
#   LUA_SRC     the Lua core and the base, coroutine, table, string, math and
#               utf8 libraries (no io, os, package, debug, no lua.c/luac.c),
#               plus lua_os_compat.c (setjmp/longjmp, Minimal-OS only)
#   SCRIPT_SRC  runtime.cpp, scan.cpp and lua_graphics.cpp (fe.graphics; needs engine/gfx/graphics_settings.cpp, which
#               $(GFX_SRC) has: link one or the other, the script tests add it themselves) (needs the ECS, so link it with
#               $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))
#
# For the editor link (host and OS) add, with the other sources:
#     $(LUA_SRC) $(SCRIPT_SRC)
# Both get their flags from the pattern rules below.
#
#   make host       builds build/host/bin/script_tests (run by make test)
#   make os         builds build/os/bin/script_smoke.run (prints FE SCRIPT PASS)

.SECONDEXPANSION:

LUA_SRC := $(addprefix external/lua/,lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldebug.c ldo.c \
             ldump.c lfunc.c lgc.c llex.c lmathlib.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c \
             lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c) engine/script/lua_os_compat.c
SCRIPT_SRC := engine/script/runtime.cpp engine/script/scan.cpp engine/script/lua_graphics.cpp

# Lua: C, warnings off. On Minimal-OS the runtime lacks setjmp.h and
# locale.h (engine/script/os_include has them) and signal.h (Lua only wants
# the type of a flag).
LUA_OS_FLAGS := -Iengine/script/os_include -Dl_signalT=int
$(BUILD)/host/external/lua/%.o: HOST_EXTRA_FLAGS += -w
$(BUILD)/os/external/lua/%.o: OS_EXTRA_FLAGS += $(LUA_OS_FLAGS) -w

# scan.cpp lists entities through flecs (for fe.find); the runtime sees
# flecs.h as a system header with the ECS defines, like engine/ecs does.
$(BUILD)/host/engine/script/%.o: HOST_EXTRA_FLAGS += $(FLECS_DEFS) $(FLECS_INC)
$(BUILD)/os/engine/script/%.o $(BUILD)/os/tests/os/script_smoke/%.o: OS_EXTRA_FLAGS += $(FLECS_OS_DEFS) $(FLECS_INC)

# ---- host tests
SCRIPT_HOST_OBJS = $(call host_objs,$(SCRIPT_SRC) engine/gfx/graphics_settings.cpp $(LUA_SRC) $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))

$(BUILD)/host/bin/script_tests: $(call host_objs,tests/host/script_tests.cpp) $$(SCRIPT_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_TESTS += $(BUILD)/host/bin/script_tests

# ---- OS smoke program
OS_OBJS_script_smoke = $(call os_objs,tests/os/script_smoke/main.cpp $(SCRIPT_SRC) engine/gfx/graphics_settings.cpp $(LUA_SRC) $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))
OS_PROGRAMS += $(BUILD)/os/bin/script_smoke.run
