# The player and game export (player/, editor/src/export.*, engine/core/pak.*, vfs.*).
#
#   build/host/bin/feplayer       headless player: feplayer game.fepak --frames N --png out.png
#   build/host/bin/fe-export      fe-export <project-or-scene> <outdir>: the editor's export without a screen
#   build/host/bin/export_tests   exports a demo project and runs the player on it (part of make test)
#   build/os/bin/feplayer.run     the player for Minimal-OS (install it next to the editor)
#
# The editor links the pak writer, the VFS, the project settings, the play
# session and the exporter too: they are added to EDITOR_SRC here.

.SECONDEXPANSION:

PAK_SRC     := engine/core/pak.cpp engine/core/vfs.cpp
EDITOR_SRC  += $(PAK_SRC) editor/src/project.cpp editor/src/play_session.cpp editor/src/export.cpp editor/src/export_ui.cpp

# What the player needs of the editor's sources: scene, scene files, project
# settings, the play session (shared with editor Play), the render bridge.
PLAYER_SHARED := editor/src/scene.cpp editor/src/scene_io.cpp editor/src/project.cpp editor/src/play_session.cpp \
                 editor/src/render_bridge.cpp $(PAK_SRC)
PLAYER_SRC    := player/player.cpp $(PLAYER_SHARED)

$(BUILD)/host/player/%.o $(BUILD)/host/tools/export/%.o $(BUILD)/host/tests/host/export_tests.cpp.o: HOST_EXTRA_FLAGS += $(EDITOR_INC)
$(BUILD)/os/player/%.o: OS_EXTRA_FLAGS += $(EDITOR_INC)

PLAYER_HOST_OBJS = $(call host_objs,$(PLAYER_SRC) $(CORE_SRC) $(ECS_SRC) $(FLECS_SRC) $(GFX_SRC) $(GFX_HOST_SRC) $(PHYSICS_SRC) \
                     $(LUA_SRC) $(SCRIPT_SRC) $(AUDIO_SRC) editor/platform/platform_headless.cpp)

$(BUILD)/host/bin/feplayer: $(call host_objs,player/main.cpp) $$(PLAYER_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

# fe-export: the exporter alone (scene loading and the pak writer; no renderer).
$(BUILD)/host/bin/fe-export: $(call host_objs,tools/export/main.cpp editor/src/export.cpp editor/src/scene.cpp editor/src/scene_io.cpp \
                                 editor/src/project.cpp $(PAK_SRC) editor/platform/platform_headless.cpp $(CORE_SRC))
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

$(BUILD)/host/bin/export_tests: $(call host_objs,tests/host/export_tests.cpp) $$(PLAYER_HOST_OBJS) $(call host_objs,editor/src/export.cpp)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_PROGRAMS += $(BUILD)/host/bin/feplayer $(BUILD)/host/bin/fe-export
HOST_TESTS    += $(BUILD)/host/bin/export_tests
# The test runs the other two programs.
$(BUILD)/host/bin/export_tests: | $(BUILD)/host/bin/feplayer $(BUILD)/host/bin/fe-export

# ---- Minimal-OS
PLAYER_OS_SRC = $(PLAYER_SRC) $(CORE_SRC) $(ECS_SRC) $(FLECS_SRC) $(GFX_SRC) $(PHYSICS_SRC) $(LUA_SRC) $(SCRIPT_SRC) $(AUDIO_SRC) \
                editor/platform/platform_minimalos.cpp player/main.cpp
OS_OBJS_feplayer = $(call os_objs,$(PLAYER_OS_SRC))
OS_PROGRAMS += $(BUILD)/os/bin/feplayer.run

# editor_tests exports a game and copies the player.
$(BUILD)/host/bin/editor_tests: | $(BUILD)/host/bin/feplayer
