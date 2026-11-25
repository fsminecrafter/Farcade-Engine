# The editor (editor/): host headless build, Minimal-OS build later.

IMGUI_SRC := external/imgui/imgui.cpp external/imgui/imgui_draw.cpp external/imgui/imgui_tables.cpp \
             external/imgui/imgui_widgets.cpp external/imgui/imgui_demo.cpp external/imguizmo/ImGuizmo.cpp
CORE_SRC  := engine/core/math.cpp engine/scene/modules.cpp engine/scene/terrain.cpp engine/script/props.cpp
EDITOR_SRC := editor/src/editor.cpp editor/src/panels.cpp editor/src/file_dialog.cpp editor/src/viewport.cpp editor/src/scene.cpp \
              editor/src/icons.cpp editor/src/swraster.cpp editor/src/view3d.cpp editor/src/widgets.cpp editor/src/render_bridge.cpp editor/src/terrain_bridge.cpp editor/src/collider_edit.cpp editor/src/scene_io.cpp editor/src/fx_bridge.cpp editor/src/icon_font.cpp

EDITOR_FONTS := fe_font_inter_regular=editor/fonts/Inter-Regular.otf \
                fe_font_inter_medium=editor/fonts/Inter-Medium.otf \
                fe_font_roboto_mono=editor/fonts/RobotoMono-Regular.ttf
FONTS_CPP := $(BUILD)/gen/fonts.cpp

$(FONTS_CPP): tools/embed.py editor/fonts/Inter-Regular.otf editor/fonts/Inter-Medium.otf editor/fonts/RobotoMono-Regular.ttf
	@mkdir -p $(dir $@)
	python3 tools/embed.py $@ $(EDITOR_FONTS)

# The icons: vector .img files (editor/icons/img, made from editor/icons/*.svg
# with tools/icons/build_pack.py convert) embedded as one table; the editor
# rasterizes them into its font atlas (editor/src/icon_font.cpp).
ICON_IMGS := $(wildcard editor/icons/img/*.img)
ICONS_CPP := $(BUILD)/gen/icon_pack.cpp

$(ICONS_CPP): tools/icons/build_pack.py editor/icons/codepoints.txt $(ICON_IMGS)
	@mkdir -p $(dir $@)
	python3 tools/icons/build_pack.py source $@

EDITOR_INC := -Ieditor -Ieditor/src -Iexternal/imgui
# icon_font.cpp decodes the icons with the SDK's image codec (mosmedia.h).
$(BUILD)/host/editor/src/icon_font.cpp.o: HOST_EXTRA_FLAGS += -idirafter $(SDK)/include
$(BUILD)/os/editor/src/icon_font.cpp.o: OS_EXTRA_FLAGS += -idirafter $(SDK)/include

$(BUILD)/host/editor/%.o $(BUILD)/host/external/imgui/%.o $(BUILD)/host/external/imguizmo/%.o: HOST_EXTRA_FLAGS += $(EDITOR_INC) -Wno-unused-function
$(BUILD)/host/editor/%.o: HOST_EXTRA_FLAGS += -Wno-missing-field-initializers
$(BUILD)/host/external/%.o: HOST_EXTRA_FLAGS += -w

$(BUILD)/host/gen/fonts.o: $(FONTS_CPP)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(LANG_CXX) -O0 -c $< -o $@

$(BUILD)/host/gen/icon_pack.o: $(ICONS_CPP)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(LANG_CXX) $(HOST_FLAGS) $(EDITOR_INC) -O0 -c $< -o $@

# mk/gfx.mk comes after this file: these lists are expanded late (= and $$).
EDITOR_HOST_OBJS = $(call host_objs,$(IMGUI_SRC) $(CORE_SRC) $(EDITOR_SRC) $(ECS_SRC) $(FLECS_SRC) $(GFX_SRC) $(GFX_HOST_SRC) $(PHYSICS_SRC) $(LUA_SRC) $(SCRIPT_SRC) $(AUDIO_SRC) $(FX_SRC) \
                     editor/platform/platform_headless.cpp) $(BUILD)/host/gen/fonts.o $(BUILD)/host/gen/icon_pack.o

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
