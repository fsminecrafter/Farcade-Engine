// C++ runtime support for Minimal-OS programs built with -fno-exceptions
// -fno-rtti: operator new/delete, pure-virtual and static-guard hooks.
// __cxa_atexit is in fe_stdlib.c (exit() runs the destructors).
#include <new>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "minimalos.h"

namespace std { const nothrow_t nothrow{}; }

extern "C" {

void* __dso_handle = &__dso_handle;

[[noreturn]] static void fe_cxx_die(const char* msg) {
    mos_write(2, msg, strlen(msg));
    abort();
}

void __cxa_pure_virtual() { fe_cxx_die("pure virtual function called\n"); }
void __cxa_deleted_virtual() { fe_cxx_die("deleted virtual function called\n"); }

// Guards of function-local statics (only used without
// -fno-threadsafe-statics). Byte 0: initialised; byte 1: in progress.
int __cxa_guard_acquire(uint64_t* g) {
    volatile uint8_t* b = (volatile uint8_t*)g;
    for (;;) {
        if (__atomic_load_n(&b[0], __ATOMIC_ACQUIRE)) return 0;
        if (!__atomic_exchange_n(&b[1], 1, __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(&b[0], __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&b[1], 0, __ATOMIC_RELEASE);
                return 0;
            }
            return 1;
        }
        mos_sleep(1);
    }
}
void __cxa_guard_release(uint64_t* g) {
    volatile uint8_t* b = (volatile uint8_t*)g;
    __atomic_store_n(&b[0], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&b[1], 0, __ATOMIC_RELEASE);
}
void __cxa_guard_abort(uint64_t* g) {
    volatile uint8_t* b = (volatile uint8_t*)g;
    __atomic_store_n(&b[1], 0, __ATOMIC_RELEASE);
}

} // extern "C"

static void* new_or_die(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) fe_cxx_die("operator new: out of memory\n");
    return p;
}
static void* new_aligned(size_t n, size_t a) {
    return aligned_alloc(a, n ? n : 1);
}
static void* new_aligned_or_die(size_t n, size_t a) {
    void* p = new_aligned(n, a);
    if (!p) fe_cxx_die("operator new: out of memory\n");
    return p;
}

void* operator new(size_t n) { return new_or_die(n); }
void* operator new[](size_t n) { return new_or_die(n); }
void* operator new(size_t n, std::align_val_t a) { return new_aligned_or_die(n, (size_t)a); }
void* operator new[](size_t n, std::align_val_t a) { return new_aligned_or_die(n, (size_t)a); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return new_aligned(n, (size_t)a); }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return new_aligned(n, (size_t)a); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free(p); }
