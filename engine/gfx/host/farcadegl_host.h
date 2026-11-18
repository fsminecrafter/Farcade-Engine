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

// The shader cache's file access (fglcache.h), with POSIX calls. The host
// build has no default directory: the cache is off until a program (or a
// test) calls fglShaderCacheSetDir().
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#define FGLC_FS_PROVIDED 1
static long fglc__fs_read(const char* path, void* buf, uint32_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}
static long fglc__fs_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long)st.st_size;
}
static int fglc__fs_delete(const char* path) { return remove(path) == 0; }
static void fglc__fs_mkdirs(const char* path) {
    char p[256];
    size_t n = strlen(path);
    if (n >= sizeof(p)) return;
    memcpy(p, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (p[i] != '/' && p[i] != 0) continue;
        char keep = p[i];
        p[i] = 0;
        mkdir(p, 0755);
        p[i] = keep;
    }
}
static int fglc__fs_write(const char* path, const void* a, uint32_t an, const void* b, uint32_t bn, const void* c,
                          uint32_t cn) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(a, 1, an, f) == an && (!bn || fwrite(b, 1, bn, f) == bn) && (!cn || fwrite(c, 1, cn, f) == cn);
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}
static void fglc__fs_list(const char* dir, void (*fn)(void* ctx, const char* name), void* ctx) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') fn(ctx, e->d_name);
    closedir(d);
}
static uint64_t fglc__fs_volume_bytes(void) { return 0; }

// The only system call FarcadeGL makes: SYS_GPU.
static inline uint64_t mos_syscall1(uint64_t number, uint64_t arg1) {
    if (number == SYS_GPU) return fe_fgpu_host_syscall((syscall_gpu_request_t*)(uintptr_t)arg1);
    return (uint64_t)-1;    // SYS_ERR_GENERIC
}

#include "farcadegl.h"
