# The audio system (engine/audio): decoders (dr_wav, dr_mp3, stb_vorbis,
# vendored unmodified in external/), AudioDevice (Minimal-OS PCM stream or
# Null), Mixer, and AudioSystem on the ECS. See docs/audio.md.
#
#   AUDIO_CORE_SRC  decoders, device and mixer (no ECS: the OS smoke program
#                   links only these)
#   AUDIO_SRC       AUDIO_CORE_SRC plus the ECS side (system.cpp, scan.cpp);
#                   link it with $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC)
#
# For the editor link (host and OS) add, with the other sources: $(AUDIO_SRC)
#
#   make host       builds build/host/bin/audio_tests (run by make test)
#   make os         builds build/os/bin/audio_smoke.run (prints FE AUDIO PASS)

# This file is read before mk/ecs.mk: lists from later files are only used in
# deferred (=) variables and second-expansion prerequisites.
.SECONDEXPANSION:

AUDIO_CORE_SRC = engine/audio/device.cpp engine/audio/mixer.cpp engine/audio/decode.c \
                 engine/audio/impl_dr_wav.c engine/audio/impl_dr_mp3.c engine/audio/impl_stb_vorbis.c
AUDIO_SRC      = $(AUDIO_CORE_SRC) engine/audio/system.cpp engine/audio/scan.cpp

# The decoders decode from memory only: no stdio paths. The units that hold
# their implementations are third-party code: warnings off.
AUDIO_C_DEFS := -DDR_WAV_NO_STDIO -DDR_MP3_NO_STDIO -DDR_MP3_NO_SIMD -DSTB_VORBIS_NO_STDIO
$(BUILD)/host/engine/audio/impl_%.o: HOST_EXTRA_FLAGS += -w
$(BUILD)/os/engine/audio/impl_%.o: OS_EXTRA_FLAGS += -w
$(BUILD)/host/engine/audio/decode.c.o: HOST_EXTRA_FLAGS += $(AUDIO_C_DEFS)
$(BUILD)/os/engine/audio/decode.c.o: OS_EXTRA_FLAGS += $(AUDIO_C_DEFS)

# scan.cpp lists entities through flecs: flecs.h as a system header with the
# ECS defines, like engine/script does.
$(BUILD)/host/engine/audio/scan.cpp.o: HOST_EXTRA_FLAGS += $(FLECS_DEFS) $(FLECS_INC)
$(BUILD)/os/engine/audio/scan.cpp.o: OS_EXTRA_FLAGS += $(FLECS_OS_DEFS) $(FLECS_INC)

# ---- host tests
AUDIO_HOST_OBJS = $(call host_objs,$(AUDIO_SRC) $(ECS_SRC) $(FLECS_SRC) $(CORE_SRC))

$(BUILD)/host/bin/audio_tests: $(call host_objs,tests/host/audio_tests.cpp) $$(AUDIO_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_TESTS += $(BUILD)/host/bin/audio_tests

# ---- OS smoke program
OS_OBJS_audio_smoke = $(call os_objs,tests/os/audio_smoke/main.cpp $(AUDIO_CORE_SRC) $(CORE_SRC))
OS_PROGRAMS += $(BUILD)/os/bin/audio_smoke.run

# The ECS side (system.cpp, scan.cpp) is not in the smoke program: compile it
# for the OS all the same.
$(BUILD)/os/engine/audio/ecs_side.stamp: $(call os_objs,engine/audio/system.cpp engine/audio/scan.cpp)
	@mkdir -p $(dir $@)
	@touch $@
OS_PROGRAMS += $(BUILD)/os/engine/audio/ecs_side.stamp
