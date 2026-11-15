# The editor (editor/): host headless build, Minimal-OS build later.

IMGUI_SRC := external/imgui/imgui.cpp external/imgui/imgui_draw.cpp external/imgui/imgui_tables.cpp \
             external/imgui/imgui_widgets.cpp external/imgui/imgui_demo.cpp external/imguizmo/ImGuizmo.cpp
CORE_SRC  := engine/core/math.cpp engine/scene/modules.cpp engine/script/props.cpp
EDITOR_SRC := editor/src/editor.cpp editor/src/panels.cpp editor/src/viewport.cpp editor/src/scene.cpp \
              editor/src/icons.cpp editor/src/swraster.cpp editor/src/view3d.cpp editor/src/widgets.cpp editor/src/render_bridge.cpp editor/src/collider_edit.cpp editor/src/scene_io.cpp

EDITOR_FONTS := fe_font_inter_regular=editor/fonts/Inter-Regular.otf \
                fe_font_inter_medium=editor/fonts/Inter-Medium.otf \
                fe_font_roboto_mono=editor/fonts/RobotoMono-Regular.ttf
FONTS_CPP := $(BUILD)/gen/fonts.cpp

$(FONTS_CPP): tools/embed.py editor/fonts/Inter-Regular.otf editor/fonts/Inter-Medium.otf editor/fonts/RobotoMono-Regular.ttf
	@mkdir -p $(dir $@)
	python3 tools/embed.py $@ $(EDITOR_FONTS)

EDITOR_INC := -Ieditor -Ieditor/src -Iexternal/imgui

$(BUILD)/host/editor/%.o $(BUILD)/host/external/imgui/%.o $(BUILD)/host/external/imguizmo/%.o: HOST_EXTRA_FLAGS += $(EDITOR_INC) -Wno-unused-function
$(BUILD)/host/editor/%.o: HOST_EXTRA_FLAGS += -Wno-missing-field-initializers
$(BUILD)/host/external/%.o: HOST_EXTRA_FLAGS += -w

$(BUILD)/host/gen/fonts.o: $(FONTS_CPP)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(LANG_CXX) -O0 -c $< -o $@

# mk/gfx.mk comes after this file: these lists are expanded late (= and $$).
EDITOR_HOST_OBJS = $(call host_objs,$(IMGUI_SRC) $(CORE_SRC) $(EDITOR_SRC) $(ECS_SRC) $(FLECS_SRC) $(GFX_SRC) $(GFX_HOST_SRC) $(PHYSICS_SRC) $(LUA_SRC) $(SCRIPT_SRC) $(AUDIO_SRC) \
                     editor/platform/platform_headless.cpp) $(BUILD)/host/gen/fonts.o

# fe-editor-shot: renders the editor with a demo scene to PNG files.
$(BUILD)/host/bin/fe-editor-shot: $$(EDITOR_HOST_OBJS) $(call host_objs,editor/main_shot.cpp)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_PROGRAMS += $(BUILD)/host/bin/fe-editor-shot

# fe-editor-bench: the editor's benchmark phases on the host (editor/bench.h).
$(BUILD)/host/bin/fe-editor-bench: $$(EDITOR_HOST_OBJS) $(call host_objs,editor/main_bench.cpp)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_PROGRAMS += $(BUILD)/host/bin/fe-editor-bench

# Interaction tests on the headless platform.
$(BUILD)/host/tests/host/%.o: HOST_EXTRA_FLAGS += $(EDITOR_INC)
$(BUILD)/host/bin/editor_tests: $$(EDITOR_HOST_OBJS) $(call host_objs,tests/host/editor_tests.cpp)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_TESTS += $(BUILD)/host/bin/editor_tests
