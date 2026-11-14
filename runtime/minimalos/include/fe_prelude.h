/* Force-included (-include) into every Minimal-OS translation unit of the
 * engine. The SDK headers thread.h, farcadegl.h, storage.h, ... say
 * #include "stdlib.h" / "string.h"; quotes search the including file's own
 * directory first, so no -I order can make them find the runtime's headers.
 * Defining the SDK's include guards here, before anything else, turns the
 * SDK's stdlib.h/string.h/stdio.h into empty files in every order. The
 * runtime's headers (found next to this file) provide everything. */
#ifndef FE_PRELUDE_H
#define FE_PRELUDE_H
#define MINIMALOS_STDLIB_H 1
#define MINIMALOS_STRING_H 1
#define MINIMALOS_STDIO_H 1
#ifndef MINIMALOS_TARGET
#define MINIMALOS_TARGET 1
#endif
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#endif
