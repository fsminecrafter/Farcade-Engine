# The editor for Minimal-OS: build/os/bin/farcade-editor.run.
# Same sources as the host build (mk/editor.mk's IMGUI_SRC, CORE_SRC,
# EDITOR_SRC and the embedded fonts), the Minimal-OS platform layer and
# editor/main_os.cpp, linked with the runtime (mk/runtime.mk).
# The lists are expanded when the link rule runs, so files added to
# mk/editor.mk are picked up whatever the include order.

EDITOR_OS_SRC = $(IMGUI_SRC) $(CORE_SRC) $(EDITOR_SRC) $(ECS_SRC) $(FLECS_SRC) $(GFX_SRC) $(PHYSICS_SRC) $(LUA_SRC) $(SCRIPT_SRC) $(AUDIO_SRC) editor/platform/platform_minimalos.cpp editor/main_os.cpp

OS_OBJS_farcade-editor = $(call os_objs,$(EDITOR_OS_SRC)) $(BUILD)/os/gen/fonts.o

$(BUILD)/os/editor/%.o: OS_EXTRA_FLAGS += $(EDITOR_INC) -Wno-unused-function -Wno-missing-field-initializers
$(BUILD)/os/engine/%.o: OS_EXTRA_FLAGS += $(EDITOR_INC)

$(BUILD)/os/gen/fonts.o: $(FONTS_CPP)
	@mkdir -p $(dir $@)
	$(OS_CXX) $(LANG_CXX) $(OS_FLAGS) -O0 -c $< -o $@

OS_PROGRAMS += $(BUILD)/os/bin/farcade-editor.run
