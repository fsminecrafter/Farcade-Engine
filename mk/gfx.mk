# The FarcadeGL renderer (engine/gfx): fe::gfx::Renderer, built for the host
# and for Minimal-OS, plus the host FGPU device that lets the SDK's
# farcadegl.h run on the PC.
#
#   GFX_SRC       renderer.cpp, passes.cpp (the GPU tier), shaders.cpp, shaders_hq.cpp (both
#                 targets; use with $(CORE_SRC))
#   GFX_HOST_SRC  the host device and the vendored FGPU CPU backend (host only;
#                 on Minimal-OS the kernel provides SYS_GPU)
#   host link:    $(call host_objs,$(GFX_SRC) $(GFX_HOST_SRC) $(CORE_SRC)) -lm
#   OS link:      $(call os_objs,$(GFX_SRC) $(CORE_SRC)); farcadegl.h comes with
#                 the runtime's include path, no extra flags
#
#   build/host/bin/gfx_tests     tests (part of make test)
#   build/host/bin/gfx_stack_tests  tests of the feature levels, the post stack, cascades and GI
#   build/host/bin/shader_compile_tests  every generated shader variant compiles
#   build/host/bin/fe-gfx-shot   fe-gfx-shot <out.png> [width height]: renders a demo scene
#   build/os/bin/gfx_smoke.run   renders a lit cube on Minimal-OS ("FE GFX PASS <base> <hq>": base tier and GPU tier checksums)

GFX_SRC      := engine/gfx/renderer.cpp engine/gfx/shaders.cpp engine/gfx/shaders_hq.cpp engine/gfx/passes.cpp engine/gfx/shader_file.cpp engine/gfx/feature_level.cpp engine/gfx/graphics_settings.cpp engine/gfx/shaders_post.cpp engine/gfx/post_stack.cpp engine/gfx/gi.cpp engine/gfx/culling.cpp engine/gfx/stb_image_impl.c
GFX_SRC      += engine/gfx/particles.cpp     # particles and sprites over the image (particles.h)
GFX_HOST_SRC := engine/gfx/host/fgpu_host.c external/fgpu/fgpu_raster.c external/fgpu/fgpu_shader.c \
                external/fgpu/fgpu_jit_x64.c external/fgpu/fgpu_ir.c

# farcadegl.h is the SDK's: its directory is on the host include path after the
# system directories only, so its stdlib.h and string.h never shadow libc's.
GFX_HOST_INC := -Iexternal/fgpu -idirafter $(SDK)/include
# The shader JIT (the kernel always has it; hosted builds opt in).
GFX_JIT_FLAG := -DFGPU_ENABLE_JIT
$(BUILD)/host/external/fgpu/%.o $(BUILD)/host/engine/gfx/host/%.o: HOST_EXTRA_FLAGS += $(GFX_JIT_FLAG)

$(BUILD)/host/engine/gfx/%.o $(BUILD)/host/external/fgpu/%.o $(BUILD)/host/tools/gfx/%.o: HOST_EXTRA_FLAGS += $(GFX_HOST_INC)
$(BUILD)/host/tests/host/gfx_tests.cpp.o $(BUILD)/host/tests/host/gfx_stack_tests.cpp.o $(BUILD)/host/tests/host/shader_compile_tests.cpp.o: HOST_EXTRA_FLAGS += $(GFX_HOST_INC)
# farcadegl.h copies matrices and vectors through one function; GCC cannot see
# that the 64 byte matrix branch is not taken for vector uniforms.
GFX_SDK_WARN := -Wno-array-bounds -Wno-stringop-overread
$(BUILD)/host/engine/gfx/renderer.cpp.o $(BUILD)/host/engine/gfx/passes.cpp.o $(BUILD)/host/tests/host/gfx_tests.cpp.o $(BUILD)/host/tests/host/gfx_stack_tests.cpp.o $(BUILD)/host/tests/host/shader_compile_tests.cpp.o: HOST_EXTRA_FLAGS += $(GFX_SDK_WARN)
$(BUILD)/os/engine/gfx/renderer.cpp.o $(BUILD)/os/engine/gfx/passes.cpp.o: OS_EXTRA_FLAGS += $(GFX_SDK_WARN)

$(BUILD)/host/tools/gfx/shot.cpp.o: HOST_EXTRA_FLAGS += -Wno-unused-function     # static stb_image_write

GFX_HOST_OBJS = $(call host_objs,$(GFX_SRC) $(GFX_HOST_SRC) $(CORE_SRC))

$(BUILD)/host/bin/gfx_tests: $(call host_objs,tests/host/gfx_tests.cpp) $(GFX_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

$(BUILD)/host/bin/gfx_stack_tests: $(call host_objs,tests/host/gfx_stack_tests.cpp) $(GFX_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

# Every generated shader variant through the FGSL front end.
$(BUILD)/host/bin/shader_compile_tests: $(call host_objs,tests/host/shader_compile_tests.cpp) $(GFX_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

$(BUILD)/host/bin/fe-gfx-shot: $(call host_objs,tools/gfx/shot.cpp) $(GFX_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -o $@ $^ -lm

HOST_PROGRAMS += $(BUILD)/host/bin/fe-gfx-shot
HOST_TESTS    += $(BUILD)/host/bin/gfx_tests $(BUILD)/host/bin/gfx_stack_tests $(BUILD)/host/bin/shader_compile_tests

# ---- Minimal-OS smoke program (run it with tools/ci/run_os.sh, see docs/runtime.md)
OS_OBJS_gfx_smoke = $(call os_objs,$(GFX_SRC) $(CORE_SRC) tests/os/gfx_smoke/main.cpp)
OS_PROGRAMS += $(BUILD)/os/bin/gfx_smoke.run
