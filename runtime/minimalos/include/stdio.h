/* stdio.h of the Minimal-OS runtime: formatted output (stb_sprintf), sscanf
 * and a small FILE API over the SDK's file calls. */
#ifndef FE_STDIO_H
#define FE_STDIO_H
#define MINIMALOS_STDIO_H 1   /* keeps the SDK's stdio.h from being read */

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fe_file FILE;

#define EOF (-1)
#define BUFSIZ 4096
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define FILENAME_MAX 4096
#define FOPEN_MAX 64
#define L_tmpnam 32

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int snprintf(char* __restrict buf, size_t n, const char* __restrict fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char* __restrict buf, size_t n, const char* __restrict fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));
int sprintf(char* __restrict buf, const char* __restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
int vsprintf(char* __restrict buf, const char* __restrict fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));
int printf(const char* __restrict fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char* __restrict fmt, va_list ap) __attribute__((format(printf, 1, 0)));
int fprintf(FILE* __restrict f, const char* __restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
int vfprintf(FILE* __restrict f, const char* __restrict fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));

int sscanf(const char* __restrict s, const char* __restrict fmt, ...);
int vsscanf(const char* __restrict s, const char* __restrict fmt, va_list ap);

FILE*  fopen(const char* path, const char* mode);
FILE*  freopen(const char* path, const char* mode, FILE* f);
int    fclose(FILE* f);
int    fflush(FILE* f);
size_t fread(void* __restrict buf, size_t size, size_t count, FILE* __restrict f);
size_t fwrite(const void* __restrict buf, size_t size, size_t count, FILE* __restrict f);
int    fseek(FILE* f, long offset, int whence);
long   ftell(FILE* f);
void   rewind(FILE* f);
int    feof(FILE* f);
int    ferror(FILE* f);
void   clearerr(FILE* f);
int    fgetc(FILE* f);
int    getc(FILE* f);
int    getchar(void);
int    ungetc(int c, FILE* f);
char*  fgets(char* __restrict buf, int n, FILE* __restrict f);
int    fputc(int c, FILE* f);
int    putc(int c, FILE* f);
int    putchar(int c);
int    fputs(const char* __restrict s, FILE* __restrict f);
int    puts(const char* s);
int    fileno(FILE* f);
int    setvbuf(FILE* f, char* buf, int mode, size_t size);
void   setbuf(FILE* f, char* buf);
int    remove(const char* path);
int    rename(const char* from, const char* to);
void   perror(const char* s);

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifdef __cplusplus
}
#endif

#ifdef MINIMALOS_TARGET
#include "fe_sdk_compat.h"
#endif

#endif
