# Terrain (engine/scene/terrain.cpp, part of $(CORE_SRC)): heightfield, brushes,
# chunk meshes, foliage scatter and the .fterrain file. See engine/scene/terrain.h.
#
#   build/host/bin/terrain_tests   tests (part of make test)

TERRAIN_TEST_OBJS = $(call host_objs,tests/host/terrain_tests.cpp $(CORE_SRC))

$(BUILD)/host/bin/terrain_tests: $$(TERRAIN_TEST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_TESTS += $(BUILD)/host/bin/terrain_tests
