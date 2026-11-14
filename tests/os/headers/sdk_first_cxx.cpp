/* Compile-only: the SDK's headers first, then the runtime's. */
#include "thread.h"
#include "farcadegl.h"
#include "window.h"
#include "storage.h"
#include "process.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <float.h>
#include <inttypes.h>
int fe_headers_sdk_first(void) {
    char* p = (char*)malloc(16);
    snprintf(p, 16, "%d", (int)sqrt(16.0));
    int r = (int)strlen(p) + (int)mos_random_u64() + (mos_heap_failed(0) ? 1 : 0) + isdigit(p[0]);
    free(p);
    return r + (int)mos_cpu_count() + (int)clock() + errno;
}
