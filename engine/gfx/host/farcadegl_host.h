// FarcadeGL on the host: includes the SDK's farcadegl.h unmodified, with
// its kernel entry point routed to the host FGPU device (fgpu_host.h).
//
// farcadegl.h includes "stdlib.h", "string.h" and "x86_64/user_syscalls.h"
// with quotes, which search the SDK's own directory first. Defining the
// SDK's include guards beforehand turns those SDK files into empty ones (the
// same trick runtime/minimalos/include/fe_prelude.h plays on Minimal-OS);
// the system libc headers are included here instead, and mos_syscall1 is
// defined below. The SDK's include directory is on the host include path
// with -idirafter only (see mk/gfx.mk), so it never shadows libc headers.
//
// Host-only: the OS build includes <farcadegl.h> directly.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MINIMALOS_STDLIB_H 1
#define MINIMALOS_STRING_H 1
#define MINIMALOS_X86_64_USER_SYSCALLS_H 1

#include "fgpu_abi.h"
#include "fgpu_host.h"

// The only system call FarcadeGL makes: SYS_GPU.
static inline uint64_t mos_syscall1(uint64_t number, uint64_t arg1) {
    if (number == SYS_GPU) return fe_fgpu_host_syscall((syscall_gpu_request_t*)(uintptr_t)arg1);
    return (uint64_t)-1;    // SYS_ERR_GENERIC
}

#include "farcadegl.h"
