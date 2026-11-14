/* One name for the stb_sprintf functions (the implementation is in
 * fe_snprintf.c, fe_stdio.c only uses the callback entry point). */
#ifndef FE_STBSP_H
#define FE_STBSP_H
#define STB_SPRINTF_DECORATE(name) fe_stbsp_##name
#endif
