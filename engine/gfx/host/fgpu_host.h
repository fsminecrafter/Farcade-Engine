// The host FGPU device: what the Minimal-OS kernel's SYS_GPU does, for the PC.
#pragma once
#include "fgpu_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Handles one SYS_GPU request (the kernel's fgpu_syscall): same operations,
// results (SYS_SUCCESS / SYS_ERR_*), limits and command semantics.
uint64_t fe_fgpu_host_syscall(syscall_gpu_request_t* r);

// Frees every surface, buffer and program and clears the statistics (tests).
void fe_fgpu_host_reset(void);

// Whether programs created from now on are compiled by the shader JIT (the
// default, as in the kernel) or run by the interpreter (identical results).
void fe_fgpu_host_set_jit(int enabled);

// Counters for tests.
typedef struct {
    uint32_t surfaces, buffers, programs;   // live objects
    uint32_t submits;                       // SYS_GPU_SUBMIT calls that succeeded
    uint32_t presents;                      // SYS_GPU_PRESENT calls (they draw nothing)
    uint64_t max_command_bytes;             // largest command buffer submitted
} fe_fgpu_host_stats_t;
void fe_fgpu_host_stats(fe_fgpu_host_stats_t* out);

#ifdef __cplusplus
}
#endif
