#ifndef FE_ALLOCA_H
#define FE_ALLOCA_H
#undef alloca
#define alloca(n) __builtin_alloca(n)
#endif
