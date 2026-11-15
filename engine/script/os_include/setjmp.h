// setjmp/longjmp for Minimal-OS (the runtime has none): x86-64, in
// engine/script/lua_os_compat.c. Only Lua's error handling uses them.
#ifndef FE_SCRIPT_SETJMP_H
#define FE_SCRIPT_SETJMP_H
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long long jmp_buf[8];     /* rbx rbp r12 r13 r14 r15 rsp rip */
int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#endif
