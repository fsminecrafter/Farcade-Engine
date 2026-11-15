# The entity system (engine/ecs): flecs v4 (external/flecs, vendored
# unmodified) plus the C++ wrapper. See docs/ecs.md.
#
# For the editor link (host and OS) add, with the other sources:
#     $(ECS_SRC) $(FLECS_SRC)          (plus $(CORE_SRC), which it needs)
# Their objects get the right flags from the pattern rules below; any other
# file that includes "flecs.h" (tests, engine internals) needs
# $(FLECS_INC) $(FLECS_DEFS): add its object directory to the rules below.
#
#   make host       builds build/host/bin/ecs_tests (run by make test)
#   make os         builds build/os/bin/ecs_smoke.run (prints FE ECS PASS)

# This file is read before mk/editor.mk, so lists defined elsewhere are only
# used in deferred (=) variables and second-expansion prerequisites.
.SECONDEXPANSION:

ECS_SRC   = engine/ecs/world.cpp
FLECS_SRC = external/flecs/flecs.c

# flecs without its OS layer (engine/ecs/world.cpp installs one), no threads,
# addons: meta (reflection), system + pipeline (the frame).
FLECS_DEFS = -DFLECS_NO_OS_API_IMPL -DFLECS_CUSTOM_BUILD -DFLECS_SYSTEM -DFLECS_PIPELINE -DFLECS_META
# On Minimal-OS flecs is told it runs on a non-glibc Linux: no execinfo, and
# the POSIX string functions are used. The runtime provides them.
FLECS_OS_DEFS = $(FLECS_DEFS) -D__linux__
FLECS_INC = -isystem external/flecs

# flecs.c: C, warnings off. The wrapper and its tests see flecs.h as a
# system header (no warnings from it) with the same defines.
$(BUILD)/host/external/flecs/%.o: HOST_EXTRA_FLAGS += $(FLECS_DEFS) -w
$(BUILD)/host/engine/ecs/%.o $(BUILD)/host/tests/host/ecs_%.o: HOST_EXTRA_FLAGS += $(FLECS_DEFS) $(FLECS_INC)
$(BUILD)/os/external/flecs/%.o: OS_EXTRA_FLAGS += $(FLECS_OS_DEFS) -w
$(BUILD)/os/engine/ecs/%.o $(BUILD)/os/tests/os/ecs_smoke/%.o: OS_EXTRA_FLAGS += $(FLECS_OS_DEFS) $(FLECS_INC)

# ---- host tests
ECS_HOST_OBJS = $(call host_objs,$(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))

$(BUILD)/host/bin/ecs_tests: $(call host_objs,tests/host/ecs_tests.cpp) $$(ECS_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_TESTS += $(BUILD)/host/bin/ecs_tests

# ---- OS smoke program
OS_OBJS_ecs_smoke = $(call os_objs,tests/os/ecs_smoke/main.cpp $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))
OS_PROGRAMS += $(BUILD)/os/bin/ecs_smoke.run
