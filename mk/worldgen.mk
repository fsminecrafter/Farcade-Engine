# The example world (docs/example-world.md): fe-worldgen builds a project from real elevation data and
# the models tools/worldgen/build.py converts.
#
#   build/host/bin/fe-worldgen      fe-worldgen --tiles DIR --out PROJECT --layers LAYERS.cfg [...]
#   make example-world              fetches the free assets, converts them and writes build/examples/are-valley
#   make example-world-lite         a 4 km map with 4 m cells and lighter models (for Minimal-OS)

WORLDGEN_OBJS := $(call host_objs,tools/worldgen/main.cpp engine/gfx/stb_image_impl.c $(CORE_SRC))
$(BUILD)/host/tools/worldgen/main.cpp.o: HOST_EXTRA_FLAGS += -Wno-unused-function     # static stb_image_write

$(BUILD)/host/bin/fe-worldgen: $(WORLDGEN_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_PROGRAMS += $(BUILD)/host/bin/fe-worldgen

.PHONY: example-world example-world-lite
example-world: $(BUILD)/host/bin/fe-worldgen $(BUILD)/host/bin/mofconv
	python3 tools/worldgen/build.py --out $(BUILD)/examples/are-valley

example-world-lite: $(BUILD)/host/bin/fe-worldgen $(BUILD)/host/bin/mofconv
	python3 tools/worldgen/build.py --lite --out $(BUILD)/examples/are-valley-lite

# worldgen_tests: fe-worldgen on made-up elevation tiles (no network), then the scene, the terrain and the player.
$(BUILD)/host/bin/worldgen_tests: $(call host_objs,tests/host/worldgen_tests.cpp) $$(PLAYER_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm
$(BUILD)/host/tests/host/worldgen_tests.cpp.o: HOST_EXTRA_FLAGS += $(EDITOR_INC) -Wno-unused-function

HOST_TESTS += $(BUILD)/host/bin/worldgen_tests
$(BUILD)/host/bin/worldgen_tests: | $(BUILD)/host/bin/fe-worldgen $(BUILD)/host/bin/feplayer
