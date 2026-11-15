# The built-in physics step (engine/physics), a stand-in for Jolt: see
# engine/physics/physics.h. Uses the ECS (mk/ecs.mk).
#
# For the editor link add $(PHYSICS_SRC) (with $(ECS_SRC) $(FLECS_SRC)).

PHYSICS_SRC = engine/physics/physics.cpp

PHYSICS_TEST_OBJS = $(call host_objs,tests/host/physics_tests.cpp $(PHYSICS_SRC) $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))

$(BUILD)/host/bin/physics_tests: $$(PHYSICS_TEST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_TESTS += $(BUILD)/host/bin/physics_tests
