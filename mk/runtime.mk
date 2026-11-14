# The Minimal-OS C/C++ runtime (runtime/minimalos): libc headers that take
# precedence over the SDK's, build/os/libfe_rt.a, the SDK's crt0, and the
# link rule for .run programs. See docs/runtime.md.
#
# Using it from another mk/*.mk fragment (any file name, any order):
#
#   OS_OBJS_myprog := $(call os_objs,src/a.cpp src/b.c)
#   OS_PROGRAMS    += $(BUILD)/os/bin/myprog.run
#
# builds build/os/bin/myprog.run (crt0 + objects + libfe_rt.a + libgcc,
# linked with the SDK's link.ld, bundled with mkrun.sh). Every OS object is
# compiled with the runtime's include directory first and fe_prelude.h
# force-included (OS_EXTRA_FLAGS below), so <stdio.h>, <math.h>, ... and the
# SDK's "minimalos.h", "thread.h", "farcadegl.h" all work together.

RT_DIR  := runtime/minimalos
RT_INC  := $(RT_DIR)/include
SDK_INC := $(SDK)/include

# The cross tools live in ~/cross/bin (SDK install-toolchain.sh); use them
# from there when they are not on PATH.
ifeq ($(shell command -v $(OS_CC) 2>/dev/null),)
FE_CROSS_BIN := $(firstword $(wildcard $(HOME)/cross/bin/x86_64-elf-gcc /root/cross/bin/x86_64-elf-gcc))
ifneq ($(FE_CROSS_BIN),)
FE_CROSS_DIR := $(patsubst %/,%,$(dir $(FE_CROSS_BIN)))
OS_CC  := $(FE_CROSS_DIR)/x86_64-elf-gcc
OS_CXX := $(FE_CROSS_DIR)/x86_64-elf-g++
OS_LD  := $(FE_CROSS_DIR)/x86_64-elf-ld
OS_AR  := $(FE_CROSS_DIR)/x86_64-elf-ar
endif
endif
OS_AR ?= x86_64-elf-ar

# Prepended, so the runtime's headers win over any -I added by other parts.
FE_RT_FLAGS    := -I$(RT_INC) -isystem $(SDK_INC) -include $(RT_INC)/fe_prelude.h
OS_EXTRA_FLAGS := $(FE_RT_FLAGS) $(OS_EXTRA_FLAGS)

# ImGui on Minimal-OS: no fork/exec shell helpers.
FE_IMGUI_OS_FLAGS := -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
$(BUILD)/os/external/imgui/%.o $(BUILD)/os/external/imguizmo/%.o: OS_EXTRA_FLAGS += $(FE_IMGUI_OS_FLAGS) -Iexternal/imgui -w

# ---- the library

RT_SRC := $(addprefix $(RT_DIR)/src/,fe_mem.c fe_string.c fe_malloc.c fe_stdlib.c fe_stdio.c \
            fe_snprintf.c fe_scanf.c fe_strtod.c fe_math.c fe_time.c fe_cxx.cpp)
RT_OBJS := $(call os_objs,$(RT_SRC))
RT_LIB  := $(BUILD)/os/libfe_rt.a
RT_CRT0 := $(BUILD)/os/rt/crt0.o
RT_LIBGCC = $(shell $(OS_CC) -print-libgcc-file-name)

# The library's own code must not be turned into calls to itself (memcpy
# loops into memcpy calls, sqrt into a call for errno), and keeps IEEE
# evaluation order.
$(BUILD)/os/$(RT_DIR)/%.o: OS_EXTRA_FLAGS += -fno-builtin -fno-math-errno -ffp-contract=off \
                                            -fno-tree-loop-distribute-patterns
$(BUILD)/os/$(RT_DIR)/src/fe_snprintf.c.o: OS_EXTRA_FLAGS += -Wno-sign-compare -Wno-implicit-fallthrough

$(RT_LIB): $(RT_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(OS_AR) rcs $@ $^

# The SDK's crt0.c, unchanged: main() is renamed so that returning from the
# program's main goes through exit() (fe_stdlib.c), and its simple memcpy &
# co. are renamed out of the way of the runtime's fast ones.
$(RT_CRT0): $(SDK)/crt0.c
	@mkdir -p $(dir $@)
	$(OS_CC) $(LANG_C) $(OS_FLAGS) -I$(SDK_INC) -fno-builtin -fno-tree-loop-distribute-patterns \
	    -Dmain=fe_crt_main -Dmemcpy=fe_sdk_crt0_memcpy -Dmemmove=fe_sdk_crt0_memmove \
	    -Dmemset=fe_sdk_crt0_memset -Dmemcmp=fe_sdk_crt0_memcmp -c $< -o $@

OS_PROGRAMS += $(RT_LIB)

# The SDK's link.ld plus .ctors (x86_64-elf-g++ puts static constructors
# there, and the SDK script only keeps .init_array: see mklinkld.py).
RT_LINK_LD := $(BUILD)/os/rt/link.ld
$(RT_LINK_LD): $(SDK)/link.ld $(RT_DIR)/mklinkld.py
	@mkdir -p $(dir $@)
	python3 $(RT_DIR)/mklinkld.py $< $@

# ---- linking .run programs: build/os/bin/NAME.run from $(OS_OBJS_NAME)

.SECONDEXPANSION:
$(BUILD)/os/bin/%.elf: $$(OS_OBJS_$$*) $(RT_LIB) $(RT_CRT0) $(RT_LINK_LD)
	@test -n "$(strip $(OS_OBJS_$*))" || { echo "OS_OBJS_$* is empty (see mk/runtime.mk)"; exit 1; }
	@mkdir -p $(dir $@)
	$(OS_LD) -pie --gc-sections --no-dynamic-linker --no-warn-rwx-segments -T $(RT_LINK_LD) -e _start \
	    -o $@ $(RT_CRT0) $(OS_OBJS_$*) $(RT_LIB) $(RT_LIBGCC)

$(BUILD)/os/bin/%.run: $(BUILD)/os/bin/%.elf $(SDK)/mkrun.sh
	$(SDK)/mkrun.sh $< $@ >/dev/null
	@echo "built $@ ($$(stat -c %s $@) bytes)"

.PRECIOUS: $(BUILD)/os/bin/%.elf

# ---- tests/os/imgui_smoke: ImGui + ImGuizmo + the runtime on Minimal-OS

RT_SMOKE_IMGUI := external/imgui/imgui.cpp external/imgui/imgui_draw.cpp external/imgui/imgui_tables.cpp \
                  external/imgui/imgui_widgets.cpp external/imgui/imgui_demo.cpp external/imguizmo/ImGuizmo.cpp
OS_OBJS_imgui_smoke := $(call os_objs,$(RT_SMOKE_IMGUI) tests/os/imgui_smoke/main.cpp)
$(BUILD)/os/tests/os/imgui_smoke/%.o: OS_EXTRA_FLAGS += -Iexternal/imgui -Iexternal/imguizmo $(FE_IMGUI_OS_FLAGS)
OS_PROGRAMS += $(BUILD)/os/bin/imgui_smoke.run

# ---- host tests: the runtime's math, strtod, sscanf and snprintf against
# glibc. The same sources are compiled for the host and every symbol they
# define is renamed to fe_<name> (objcopy), so they link next to glibc.

RT_HOST_UNITS := fe_math fe_strtod fe_scanf fe_snprintf
RT_HOST_RAW   := $(patsubst %,$(BUILD)/host/rt/%.raw.o,$(RT_HOST_UNITS))
RT_HOST_OBJS  := $(patsubst %,$(BUILD)/host/rt/%.o,$(RT_HOST_UNITS))

$(BUILD)/host/rt/%.raw.o: $(RT_DIR)/src/%.c $(wildcard $(RT_INC)/*.h) $(wildcard $(RT_DIR)/src/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(LANG_C) -O2 -fno-builtin -fno-math-errno -ffp-contract=off -w \
	    -Iexternal -I$(RT_INC) -c $< -o $@

$(BUILD)/host/rt/syms.txt: $(RT_HOST_RAW)
	nm --defined-only -g $^ | awk 'NF == 3 { print $$3 " fe_" $$3 }' | sort -u > $@

$(BUILD)/host/rt/%.o: $(BUILD)/host/rt/%.raw.o $(BUILD)/host/rt/syms.txt
	objcopy --redefine-syms=$(BUILD)/host/rt/syms.txt $< $@

$(BUILD)/host/bin/rt_math_test: $(call host_objs,tests/os/host/rt_math_test.c) $(RT_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^ -lm

$(BUILD)/host/bin/rt_text_test: $(call host_objs,tests/os/host/rt_text_test.c) $(RT_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^ -lm

$(BUILD)/host/tests/os/host/%.o: HOST_EXTRA_FLAGS += -Wno-format -Wno-unused-result

HOST_TESTS += $(BUILD)/host/bin/rt_math_test $(BUILD)/host/bin/rt_text_test

# ---- compile-only: the SDK's headers before and after the runtime's, C and C++
RT_HEADER_TESTS := $(call os_objs,$(wildcard tests/os/headers/*.c tests/os/headers/*.cpp))
$(BUILD)/os/rt/headers.stamp: $(RT_HEADER_TESTS)
	@mkdir -p $(dir $@)
	@touch $@
OS_PROGRAMS += $(BUILD)/os/rt/headers.stamp
