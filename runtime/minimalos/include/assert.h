/* assert.h: no include guard, as the standard says (NDEBUG may change). */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((noreturn)) void __fe_assert_fail(const char* expr, const char* file, int line, const char* func);
#ifdef __cplusplus
}
#endif
#define assert(e) ((e) ? (void)0 : __fe_assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
