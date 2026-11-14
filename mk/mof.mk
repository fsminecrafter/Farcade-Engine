# .mof model format: reader/writer library, importers, mofconv, tests.
#
#   make host            builds mofconv, mofgen and the tests (build/host/bin)
#   make test            runs mof_tests and mof_import_tests with the others
#   make test-mof-asan   same tests rebuilt with -fsanitize=address,undefined
#   make mof-samples     regenerates assets/samples/*.mof with mofgen

# Core: freestanding-clean (also built for Minimal-OS later).
MOF_CORE_SRCS := engine/mof/mof_read.cpp engine/mof/mof_write.cpp
# Importers: host libc; cgltf / stb / fast_obj implementations are plain C files.
MOF_IMPORT_SRCS := engine/mof/import_common.cpp engine/mof/import_gltf.cpp engine/mof/import_obj.cpp
MOF_IMPORT_CSRCS := engine/mof/mof_ext_impl.c external/fast_obj/fast_obj.c
MOF_TOOL_SRCS := tools/mofconv/mof_info.cpp tools/mofconv/samples.cpp

MOF_CORE_OBJS   := $(call host_objs,$(MOF_CORE_SRCS))
MOF_IMPORT_OBJS := $(call host_objs,$(MOF_IMPORT_SRCS) $(MOF_IMPORT_CSRCS))
MOF_TOOL_OBJS   := $(call host_objs,$(MOF_TOOL_SRCS))
MOF_BIN         := $(BUILD)/host/bin

$(MOF_BIN)/mofconv: $(call host_objs,tools/mofconv/main.cpp) $(MOF_TOOL_OBJS) $(MOF_CORE_OBJS) $(MOF_IMPORT_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_EXTRA_FLAGS) -o $@ $^ -lm

$(MOF_BIN)/mofgen: $(call host_objs,tools/mofconv/mofgen.cpp) $(MOF_TOOL_OBJS) $(MOF_CORE_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_EXTRA_FLAGS) -o $@ $^ -lm

$(MOF_BIN)/mof_tests: $(call host_objs,tests/host/mof_tests.cpp) $(MOF_TOOL_OBJS) $(MOF_CORE_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_EXTRA_FLAGS) -o $@ $^ -lm

$(MOF_BIN)/mof_import_tests: $(call host_objs,tests/host/mof_import_tests.cpp) $(MOF_TOOL_OBJS) $(MOF_CORE_OBJS) $(MOF_IMPORT_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_EXTRA_FLAGS) -o $@ $^ -lm

HOST_PROGRAMS += $(MOF_BIN)/mofconv $(MOF_BIN)/mofgen
HOST_TESTS    += $(MOF_BIN)/mof_tests $(MOF_BIN)/mof_import_tests

.PHONY: test-mof-asan mof-samples

# Rebuild the mof tests with sanitizers in build/asan (recursive make, so the
# normal rules are reused with different flags) and run them.
MOF_ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
test-mof-asan:
	$(MAKE) BUILD=$(BUILD)/asan HOST_EXTRA_FLAGS='$(MOF_ASAN_FLAGS)' \
	    $(BUILD)/asan/host/bin/mof_tests $(BUILD)/asan/host/bin/mof_import_tests
	$(BUILD)/asan/host/bin/mof_tests
	$(BUILD)/asan/host/bin/mof_import_tests
	@echo "MOF ASAN/UBSAN TESTS PASSED"

mof-samples: $(MOF_BIN)/mofgen
	$(MOF_BIN)/mofgen assets/samples
