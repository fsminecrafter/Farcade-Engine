# Farcade Engine
#
#   make host      Linux build: libraries, tools, tests and the editor with a
#                  headless platform (renders frames to PNG files)
#   make os        Minimal-OS build: .run programs in build/os/bin
#   make test      host tests
#   make clean
#
# Both targets use the same language rules: C++20, no exceptions, no RTTI,
# no standard C++ library, so code that builds on the host builds for the OS.
# Each part of the engine adds itself in its own mk/*.mk file.

BUILD    ?= build
SDK      := external/Minimal-OS-SDK

HOST_CC  ?= gcc
HOST_CXX ?= g++
OS_CC    ?= x86_64-elf-gcc
OS_CXX   ?= x86_64-elf-g++
OS_LD    ?= x86_64-elf-ld

WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LANG_CXX := -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics
LANG_C   := -std=gnu11

HOST_FLAGS := -O2 -g -DFE_HOST=1 -Iengine -Iexternal $(WARN)
OS_FLAGS   := -O2 -ffunction-sections -fdata-sections -ffreestanding \
              -fno-stack-protector -fno-asynchronous-unwind-tables \
              -fno-unwind-tables -fPIE -DFE_MINIMALOS=1 -DMINIMALOS_TARGET \
              -Iengine -Iexternal $(WARN)

# Filled in by the mk/*.mk files.
HOST_PROGRAMS :=
HOST_TESTS    :=
OS_PROGRAMS   :=

.PHONY: all host os test clean
all: host

# build/host/<path>.o and build/os/<path>.o for a list of sources.
host_objs = $(patsubst %,$(BUILD)/host/%.o,$(1))
os_objs   = $(patsubst %,$(BUILD)/os/%.o,$(1))

include $(sort $(wildcard mk/*.mk))

host: $(HOST_PROGRAMS) $(HOST_TESTS)
os: $(OS_PROGRAMS)

test: $(HOST_TESTS)
	@set -e; for t in $(HOST_TESTS); do echo "== $$t"; $$t; done; echo "ALL TESTS PASSED"

# ---- compile rules: build/host/<path>.o and build/os/<path>.o

$(BUILD)/host/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(LANG_CXX) $(HOST_FLAGS) $(HOST_EXTRA_FLAGS) -MMD -MP -c $< -o $@

$(BUILD)/host/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(LANG_C) $(HOST_FLAGS) $(HOST_EXTRA_FLAGS) -MMD -MP -c $< -o $@

$(BUILD)/os/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(OS_CXX) $(LANG_CXX) $(OS_FLAGS) $(OS_EXTRA_FLAGS) -MMD -MP -c $< -o $@

$(BUILD)/os/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(OS_CC) $(LANG_C) $(OS_FLAGS) $(OS_EXTRA_FLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
