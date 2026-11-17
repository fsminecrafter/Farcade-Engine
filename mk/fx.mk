# Effects and gameplay modules (engine/fx): particles, force fields, spawners,
# animation, billboards and text. They run on the ECS (mk/ecs.mk) and are
# started by the host (the editor's Play, a game player) through fe::FxWorld
# (engine/fx/fx.h).
#
#   FX_SRC        the fx systems (forcefield.cpp is in PHYSICS_SRC: the physics
#                 step needs it)
#   FX_TEST_OBJS  what a test linking the fx systems needs
#
# For the editor (or a player) link add $(FX_SRC) with the other sources.
#
#   make host     builds build/host/bin/fx_tests (run by make test)

.SECONDEXPANSION:

FX_SRC = engine/fx/fx.cpp engine/fx/particles.cpp engine/fx/spawner.cpp engine/fx/scan.cpp

# scan.cpp lists entities through flecs: flecs.h as a system header with the
# ECS defines, like engine/audio does.
$(BUILD)/host/engine/fx/scan.cpp.o: HOST_EXTRA_FLAGS += $(FLECS_DEFS) $(FLECS_INC)
$(BUILD)/os/engine/fx/scan.cpp.o: OS_EXTRA_FLAGS += $(FLECS_OS_DEFS) $(FLECS_INC)
$(BUILD)/host/tests/host/fx_tests.cpp.o: HOST_EXTRA_FLAGS += $(FLECS_DEFS) $(FLECS_INC)

# ---- host tests
FX_TEST_OBJS = $(call host_objs,$(FX_SRC) $(PHYSICS_SRC) engine/gfx/particles.cpp engine/gfx/stb_image_impl.c $(SCRIPT_SRC) $(LUA_SRC) $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))

$(BUILD)/host/bin/fx_tests: $(call host_objs,tests/host/fx_tests.cpp) $$(FX_TEST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_TESTS += $(BUILD)/host/bin/fx_tests
