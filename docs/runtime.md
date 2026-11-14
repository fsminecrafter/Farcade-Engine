# The Minimal-OS runtime

`runtime/minimalos/` is the small C/C++ runtime the engine and the editor link
on Minimal-OS. The SDK's own `stdlib.h`/`stdio.h`/`string.h` are tiny inline
helpers, and its `malloc()` is one system call per block on the kernel heap;
the runtime replaces them with a libc subset that Dear ImGui, ImGuizmo,
stb_image, stb_truetype, cgltf and fast_obj need, while the SDK's other
headers (`minimalos.h`, `thread.h`, `vm.h`, `window.h`, `farcadegl.h`,
`process.h`, ...) keep working in the same translation unit.

## Using it

In a `mk/*.mk` fragment (any name, any order relative to `mk/runtime.mk`):

```make
OS_OBJS_mytool := $(call os_objs,tools/mytool/main.cpp engine/core/math.cpp)
OS_PROGRAMS    += $(BUILD)/os/bin/mytool.run
```

`make os` then builds `build/os/bin/mytool.run`: the SDK's `crt0.c`, the
objects, `build/os/libfe_rt.a` and libgcc, linked with the SDK's `link.ld`
(plus `.ctors`, see below) and bundled with the SDK's `mkrun.sh`. The ELF
stays next to it (`mytool.elf`). The link rule is a pattern rule with
secondary expansion, so `OS_OBJS_<name>` may be set after or before
`mk/runtime.mk` is read; set it with `=` if it uses variables of other
fragments (as `mk/editor_os.mk` does with `EDITOR_SRC`).

Every Minimal-OS object gets these flags from `mk/runtime.mk` (prepended to
`OS_EXTRA_FLAGS`):

```
-Iruntime/minimalos/include -isystem external/Minimal-OS-SDK/include
-include runtime/minimalos/include/fe_prelude.h
```

Write code as on the host: `#include <stdio.h>`, `<math.h>`, `<new>`, ... and
the SDK headers by name (`"minimalos.h"`, `"thread.h"`). If the cross tools
are not on `PATH`, `mk/runtime.mk` takes them from `~/cross/bin`.

Running a program on Minimal-OS in QEMU:

```sh
tools/ci/run_os.sh build/os/bin/imgui_smoke.run 'FE SMOKE PASS' 'FE SMOKE FAIL' editor/fonts/Inter-Regular.otf
SCREENDUMP=build/os/editor-os.png tools/ci/run_os.sh build/os/bin/farcade-editor.run 'FE EDITOR frames 121'
```

It stages the files in `$MINIMAL_OS_DIR/src/resources/install2/` (they end up
in `0:/programs/`), builds the ISO there, boots it headless, types
`run 0:/programs/<name>.run $ARGS` on the serial line and waits for the
pass/fail regex. The header of the script lists the options (`QEMU_EXTRA`,
`QEMU_SMP`, `TIMEOUT`, `SCREENDUMP`, `MONITOR_CMDS`, ...). The serial and
monitor logs go to `build/os/run_os/`.

## Why the prelude

The SDK headers include `"stdlib.h"`, `"string.h"` with quotes, and quotes
search the including file's directory first, so no `-I` order can make
`thread.h` or `farcadegl.h` see the runtime's headers. `fe_prelude.h` defines
the SDK's include guards (`MINIMALOS_STDLIB_H`, `MINIMALOS_STRING_H`,
`MINIMALOS_STDIO_H`) before anything else, which turns those three SDK files
into empty files, and includes the runtime's versions. What the SDK versions
defined besides libc functions is still there: `mos_heap_failed`,
`mos_random_bytes`, `mos_random_u64`, `mos_vprintf` (`fe_sdk_compat.h`),
`strlcpy_safe`, `mos_memcmp`, `mos_memeq_ct` (`string.h`).
`tests/os/headers/` compiles `thread.h`, `farcadegl.h`, `window.h`,
`storage.h`, `process.h` before and after the runtime's headers, in C and C++,
and stb_image, stb_image_write, stb_truetype, cgltf and fast_obj against the
runtime (every symbol they need is in `libfe_rt.a`); `make os` builds them.

## What it provides

| Header | Contents |
|---|---|
| `string.h`, `strings.h` | mem*/str* (SSE2 and `rep movsb`/`stosb` for memcpy/memmove/memset), strtok(_r), strdup, strndup, strlcpy, strcasecmp, strerror, ... |
| `stdlib.h`, `malloc.h`, `alloca.h` | malloc/calloc/realloc/free/aligned_alloc/posix_memalign/memalign/malloc_usable_size, `fe_malloc_stats()`; strtod/strtof/strtol/strtoul/strtoll/strtoull/atoi/atol/atof; qsort (stable merge sort), bsearch; abs/div; rand; getenv (the program's environment); exit/atexit/abort/_Exit |
| `stdio.h` | snprintf/vsnprintf/sprintf/vsprintf (stb_sprintf), printf/fprintf/vfprintf, sscanf/vsscanf; FILE: fopen/freopen/fclose/fflush/fread/fwrite/fseek/ftell/rewind/feof/ferror/fgetc/fgets/ungetc/fputc/fputs/puts, remove, rename, perror; stdin/stdout/stderr |
| `math.h` | float and double: sqrt cbrt sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh exp exp2 expm1 log log2 log10 log1p logb pow fmod remainder hypot floor ceil round trunc rint nearbyint lrint lround fabs fmin fmax fdim copysign nextafter fma ldexp scalbn frexp modf ilogb nan; isnan/isinf/isfinite/signbit/fpclassify, HUGE_VAL, INFINITY, NAN, M_PI ... |
| `time.h` | clock (monotonic µs), time, clock_gettime, nanosleep, usleep, gmtime(_r), localtime(_r) (= UTC), mktime, timegm, strftime |
| `ctype.h`, `assert.h`, `errno.h`, `limits.h`, `inttypes.h`, `sys/types.h`, `new` | as usual; `limits.h` adds PATH_MAX to GCC's |
| from GCC | `stdint.h`, `stddef.h`, `stdarg.h`, `stdbool.h`, `float.h`, intrinsics |

C++ support (`fe_cxx.cpp`): all forms of `operator new`/`delete` (sized,
aligned, nothrow; plain `new` aborts with a message when memory runs out,
there are no exceptions), `__cxa_pure_virtual`, `__cxa_atexit` (static
destructors run in `exit()`), `__dso_handle`, `__cxa_guard_*`. libgcc is
linked for 128-bit arithmetic and the like.

### Start and exit

`crt0.c` is the SDK's, compiled with `-Dmain=fe_crt_main` (and its simple
memcpy & co. renamed out of the way). `fe_crt_main` calls `main` and then
`exit()`, so returning from `main` runs `atexit` handlers and static
destructors and flushes open `FILE`s. Static constructors run before `main`:
x86_64-elf-g++ puts them in `.ctors`, which the SDK's `link.ld` does not
collect, so the link uses `build/os/rt/link.ld`, the SDK script with `.ctors`
added to `.init_array` (`runtime/minimalos/mklinkld.py`). Without this, C++
globals with constructors stay zero; programs built with the SDK's
`build.sh` and g++ have that problem today.

### malloc

`fe_malloc.c`: memory comes from `SYS_VM` in 4 MiB chunks (demand-zero, so an
untouched chunk costs no physical memory). Blocks up to 64 KiB are served from
44 size classes (16-byte steps to 128 bytes, then four per doubling), each
with its own futex lock, free list and bump span; freed small blocks are
reused but not returned to the kernel. Bigger blocks are mappings of their
own and are unmapped by `free()`. Every block has a 16-byte header (16-byte
alignment; `free()` of a bad or already freed pointer aborts with a
message). There are no per-thread caches: threads allocating the same size
class at the same time take turns on its lock. Pointers are above 2 TiB,
which the kernel calls accept (SYS_READ, blits, ...).

### Accuracy and deviations

- Math (host tests against glibc, `make test`): sqrt, floor/ceil/round/trunc,
  fmod, frexp/ldexp/modf exact; sin/cos/exp/log/atan/atan2/exp2/log2/log10/pow
  within 1 ulp (pow, log2, log10 and exp2 use double-double and are exact for
  exact cases such as `pow(10, 22)` and `log10(1000)`); tan, asin, acos,
  log1p and the hyperbolics within 2-3 ulp. Trigonometry reduces huge arguments exactly
  (Payne-Hanek). Float functions evaluate the double function and round.
  No x87 and no FMA instructions are used.
- strtod is correctly rounded (19 significant digits are kept; the scaling is
  done in double-double), including subnormals and hex floats.
- printf is stb_sprintf: exact to about 16 significant digits (`%.17g` may
  differ from glibc in the last digit), ties round half up (`%.0f` of 2.5 is
  "3"), `%p` prints 16 hex digits without `0x`. Floats (`%.9g`) round-trip.
- sscanf: `%d %i %u %o %x %X %c %s %[set] %e %f %g %a %p %n %%`, widths, `*`,
  `hh h l ll j z t L`. No `fscanf`.
- FILE: one 4 KiB buffer per file; "w" deletes and re-creates the file
  (MinimaFS writes never shorten a file); data written reaches the disk when
  the file is closed (or at exit). FILE objects are not locked.
- `errno` is one variable for the whole program (no TLS on Minimal-OS);
  `localtime` is UTC; no locales, no wide characters, no signals, no
  `setjmp`.

## The editor on Minimal-OS

`mk/editor_os.mk` builds `build/os/bin/farcade-editor.run` from mk/editor.mk's
`IMGUI_SRC`, `CORE_SRC`, `EDITOR_SRC`, the embedded fonts,
`editor/platform/platform_minimalos.cpp` and `editor/main_os.cpp`. It prints
`FE EDITOR READY` after the first frame and the average frame time every 60
frames; `--frames N` quits after N frames (`FE EDITOR DONE`).

## Tests

- `make test` runs `build/host/bin/rt_math_test` and `rt_text_test`: the
  runtime's math, strtod/strtol, sscanf and snprintf sources compiled for the
  host, every defined symbol renamed to `fe_<name>` with objcopy, compared
  with glibc (special values, random arguments over wide ranges, ImGui's
  own sscanf formats).
- `tests/os/imgui_smoke` (`build/os/bin/imgui_smoke.run`): libm and text
  checks against values from the PC, FILE I/O, malloc/realloc/aligned_alloc
  from two threads, a static constructor, ImGui with docking, the default
  font and Inter loaded from `0:/programs/`, five frames of the demo window,
  one `ImGuizmo::Manipulate`, the draw data, the ini round trip in memory and
  on disk. Prints `FE SMOKE PASS`.
