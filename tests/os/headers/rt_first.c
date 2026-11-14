/* Compile-only: the runtime's headers first, then the SDK's. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "minimalos.h"
#include "vm.h"
#include "thread.h"
#include "window.h"
#include "farcadegl.h"
int fe_headers_rt_first(void) {
    void* p = mos_vm_map(NULL, 4096, MOS_VM_PROT_RW, 0);
    char buf[8];
    strlcpy_safe(buf, "abc", sizeof buf);
    return (p != NULL) + mos_memeq_ct(buf, "abc", 3) + (int)printf("%s\n", buf) + (int)floorf(1.5f);
}
