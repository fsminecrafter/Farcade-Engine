#ifndef FE_STRINGS_H
#define FE_STRINGS_H
#include <string.h>
#define bzero(p, n) memset((p), 0, (n))
#define bcopy(s, d, n) memmove((d), (s), (n))
#endif
