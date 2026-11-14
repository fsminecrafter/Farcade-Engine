#ifndef FE_LIMITS_H
#define FE_LIMITS_H
#include_next <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif
#endif
