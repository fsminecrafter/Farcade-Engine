/*
 * FILE API on the SDK's file calls (minimalos.h), and printf to
 * stdout/stderr.
 *
 * MinimaFS rules this layer hides: a file must be created before it can be
 * opened, writing never shortens a file ("w" deletes and re-creates it), a
 * seek past the end is clamped, and what was written reaches the disk when
 * the handle is closed. A handle is a 64-bit value kept in a long.
 *
 * Each FILE has one 4 KiB buffer used for reading or for pending writes.
 * stdout and stderr are written straight through (fd 1 and 2). A FILE is
 * not locked: use one from one thread at a time (printf from several
 * threads is fine, the lines may interleave).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "minimalos.h"
#include "fe_stbsp.h"
#include "stb/stb_sprintf.h"

#define F_READ   1u
#define F_WRITE  2u
#define F_APPEND 4u
#define F_EOF    8u
#define F_ERR    16u
#define F_STD    32u

struct fe_file {
    long fd;
    unsigned flags;
    int ungot;                  /* pushed-back character or -1 */
    size_t rpos, rlen;          /* unread bytes are buf[rpos..rlen) */
    size_t wlen;                /* bytes waiting to be written */
    struct fe_file* next;
    struct fe_file* prev;
    unsigned char buf[BUFSIZ];
};

static struct fe_file g_stdin  = { 0, F_READ | F_STD, -1, 0, 0, 0, 0, 0, { 0 } };
static struct fe_file g_stdout = { 1, F_WRITE | F_STD, -1, 0, 0, 0, 0, 0, { 0 } };
static struct fe_file g_stderr = { 2, F_WRITE | F_STD, -1, 0, 0, 0, 0, 0, { 0 } };
FILE* stdin = &g_stdin;
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;

static struct fe_file* g_open_files;
static volatile int g_list_lock;
static void list_lock(void) {
    while (__atomic_exchange_n(&g_list_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}
static void list_unlock(void) { __atomic_store_n(&g_list_lock, 0, __ATOMIC_RELEASE); }

static int flush_write(FILE* f) {
    if (!f->wlen) return 0;
    size_t n = f->wlen;
    f->wlen = 0;
    if (f->flags & F_APPEND) mos_seek(f->fd, mos_size(f->fd));
    long w = mos_fwrite(f->fd, f->buf, n);
    if (w != (long)n) { f->flags |= F_ERR; errno = EIO; return EOF; }
    return 0;
}

/* Drop read-ahead: put the kernel's position back to where the program is. */
static void drop_read(FILE* f) {
    if (f->rlen > f->rpos) {
        long pos = mos_tell(f->fd);
        long back = (long)(f->rlen - f->rpos);
        if (pos >= back) mos_seek(f->fd, pos - back);
    }
    f->rpos = f->rlen = 0;
    f->ungot = -1;
}

static long open_fd(const char* path, const char* mode, unsigned* flags_out) {
    int rd = 0, wr = 0, ap = 0, plus = 0;
    for (const char* m = mode; *m; m++) {
        if (*m == 'r') rd = 1;
        else if (*m == 'w') wr = 1;
        else if (*m == 'a') ap = 1;
        else if (*m == '+') plus = 1;
    }
    if (!path || !*path || rd + wr + ap != 1) { errno = EINVAL; return -1; }
    long fd;
    if (rd) {
        fd = mos_open(path, plus ? SYS_O_RDWR : SYS_O_RDONLY);
        if (fd <= 0) { errno = ENOENT; return -1; }
        *flags_out = F_READ | (plus ? F_WRITE : 0);
        return fd;
    }
    if (wr && mos_exists(path) > 0) {
        if (mos_is_dir(path) > 0) { errno = EISDIR; return -1; }
        mos_delete(path);
    }
    if (mos_exists(path) <= 0) {
        if (mos_create(path, NULL, NULL) != 0) { errno = ENOENT; return -1; }
    }
    fd = mos_open(path, SYS_O_RDWR);
    if (fd <= 0) { errno = EIO; return -1; }
    if (ap) mos_seek(fd, mos_size(fd));
    *flags_out = F_WRITE | (plus ? F_READ : 0) | (ap ? F_APPEND : 0);
    return fd;
}

FILE* fopen(const char* path, const char* mode) {
    unsigned flags = 0;
    long fd = open_fd(path, mode, &flags);
    if (fd <= 0) return NULL;
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) { mos_close(fd); errno = ENOMEM; return NULL; }
    f->fd = fd;
    f->flags = flags;
    f->ungot = -1;
    f->rpos = f->rlen = f->wlen = 0;
    f->prev = NULL;
    list_lock();
    f->next = g_open_files;
    if (g_open_files) g_open_files->prev = f;
    g_open_files = f;
    list_unlock();
    return f;
}

FILE* freopen(const char* path, const char* mode, FILE* f) {
    if (!f || (f->flags & F_STD) || !path) return NULL;
    flush_write(f);
    mos_close(f->fd);
    unsigned flags = 0;
    long fd = open_fd(path, mode, &flags);
    if (fd <= 0) {
        f->fd = 0; f->flags = F_ERR;
        return NULL;
    }
    f->fd = fd; f->flags = flags; f->ungot = -1;
    f->rpos = f->rlen = f->wlen = 0;
    return f;
}

int fflush(FILE* f) {
    if (!f) {
        int r = 0;
        list_lock();
        for (FILE* p = g_open_files; p; p = p->next) if (flush_write(p)) r = EOF;
        list_unlock();
        return r;
    }
    if (f->flags & F_STD) return 0;
    return flush_write(f);
}

/* At exit: write out and close every open file (MinimaFS saves a file when
 * its handle is closed). The FILE objects stay allocated. */
void fe_stdio_exit(void) {
    list_lock();
    for (FILE* p = g_open_files; p; p = p->next) {
        flush_write(p);
        if (p->fd > 0) mos_close(p->fd);
        p->fd = 0;
        p->flags = F_ERR;
    }
    g_open_files = NULL;
    list_unlock();
}

int fclose(FILE* f) {
    if (!f) return EOF;
    if (f->flags & F_STD) return 0;
    int r = flush_write(f);
    if (f->fd > 0) mos_close(f->fd);
    list_lock();
    if (f->prev) f->prev->next = f->next; else g_open_files = f->next;
    if (f->next) f->next->prev = f->prev;
    list_unlock();
    free(f);
    return r;
}

size_t fread(void* __restrict bufp, size_t size, size_t count, FILE* __restrict f) {
    unsigned char* out = (unsigned char*)bufp;
    size_t total;
    if (__builtin_mul_overflow(size, count, &total) || !total) return 0;
    if (!(f->flags & F_READ) || (f->flags & F_STD)) { f->flags |= F_EOF; return 0; }
    if (f->wlen) flush_write(f);
    size_t done = 0;
    if (f->ungot >= 0) { out[done++] = (unsigned char)f->ungot; f->ungot = -1; }
    while (done < total) {
        size_t avail = f->rlen - f->rpos;
        if (avail) {
            size_t n = total - done < avail ? total - done : avail;
            memcpy(out + done, f->buf + f->rpos, n);
            f->rpos += n;
            done += n;
            continue;
        }
        size_t left = total - done;
        long r;
        if (left >= BUFSIZ) {
            /* big read: straight into the caller's memory */
            r = mos_read(f->fd, out + done, left > 0x40000000u ? 0x40000000u : left);
            if (r > 0) { done += (size_t)r; continue; }
        } else {
            f->rpos = 0; f->rlen = 0;
            r = mos_read(f->fd, f->buf, BUFSIZ);
            if (r > 0) { f->rlen = (size_t)r; continue; }
        }
        if (r < 0) { f->flags |= F_ERR; errno = EIO; }
        else f->flags |= F_EOF;
        break;
    }
    return done / size;
}

static size_t write_bytes(FILE* f, const unsigned char* in, size_t total) {
    if (!(f->flags & F_WRITE)) { f->flags |= F_ERR; errno = EBADF; return 0; }
    if (f->flags & F_STD) {
        size_t done = 0;
        while (done < total) {
            size_t n = total - done > 4096 ? 4096 : total - done;
            mos_write((int)f->fd, in + done, n);
            done += n;
        }
        return total;
    }
    if (f->rlen || f->ungot >= 0) drop_read(f);
    if (total >= BUFSIZ) {
        if (flush_write(f)) return 0;
        if (f->flags & F_APPEND) mos_seek(f->fd, mos_size(f->fd));
        long w = mos_fwrite(f->fd, in, total);
        if (w != (long)total) { f->flags |= F_ERR; errno = EIO; return w > 0 ? (size_t)w : 0; }
        return total;
    }
    if (f->wlen + total > BUFSIZ && flush_write(f)) return 0;
    memcpy(f->buf + f->wlen, in, total);
    f->wlen += total;
    return total;
}

size_t fwrite(const void* __restrict bufp, size_t size, size_t count, FILE* __restrict f) {
    size_t total;
    if (__builtin_mul_overflow(size, count, &total) || !total) return 0;
    return write_bytes(f, (const unsigned char*)bufp, total) / size;
}

long ftell(FILE* f) {
    if (f->flags & F_STD) { errno = ESPIPE; return -1; }
    long pos = mos_tell(f->fd);
    if (pos < 0) { errno = EIO; return -1; }
    pos -= (long)(f->rlen - f->rpos);
    if (f->ungot >= 0 && pos > 0) pos--;
    return pos + (long)f->wlen;
}

int fseek(FILE* f, long offset, int whence) {
    if (f->flags & F_STD) { errno = ESPIPE; return -1; }
    long base;
    if (flush_write(f)) return -1;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = ftell(f);
    else if (whence == SEEK_END) base = mos_size(f->fd);
    else { errno = EINVAL; return -1; }
    long target = base + offset;
    if (base < 0 || target < 0) { errno = EINVAL; return -1; }
    f->rpos = f->rlen = 0;
    f->ungot = -1;
    if (mos_seek(f->fd, target) != 0) { errno = EIO; return -1; }
    f->flags &= ~F_EOF;
    return 0;
}

void rewind(FILE* f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int feof(FILE* f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE* f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE* f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE* f) { return (int)f->fd; }
int setvbuf(FILE* f, char* buf, int mode, size_t size) { (void)f; (void)buf; (void)mode; (void)size; return 0; }
void setbuf(FILE* f, char* buf) { (void)f; (void)buf; }

int fgetc(FILE* f) {
    if (f->ungot < 0 && f->rpos < f->rlen && !f->wlen) return f->buf[f->rpos++];
    unsigned char c;
    return fread(&c, 1, 1, f) == 1 ? (int)c : EOF;
}
int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE* f) {
    if (c == EOF || f->ungot >= 0) return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

char* fgets(char* __restrict buf, int n, FILE* __restrict f) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = 0;
    return buf;
}

int fputc(int c, FILE* f) {
    unsigned char ch = (unsigned char)c;
    return write_bytes(f, &ch, 1) == 1 ? (int)ch : EOF;
}
int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char* __restrict s, FILE* __restrict f) {
    size_t n = strlen(s);
    return write_bytes(f, (const unsigned char*)s, n) == n ? 0 : EOF;
}

int puts(const char* s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

/* ---- formatted output ----------------------------------------------- */

typedef struct { FILE* f; char tmp[STB_SPRINTF_MIN]; } out_ctx_t;

static char* out_cb(const char* buf, void* user, int len) {
    out_ctx_t* c = (out_ctx_t*)user;
    if (len > 0) write_bytes(c->f, (const unsigned char*)buf, (size_t)len);
    return c->tmp;
}

int vfprintf(FILE* __restrict f, const char* __restrict fmt, va_list ap) {
    out_ctx_t c;
    c.f = f;
    return fe_stbsp_vsprintfcb(out_cb, &c, c.tmp, fmt, ap);
}

int fprintf(FILE* __restrict f, const char* __restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char* __restrict fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char* __restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

void perror(const char* s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

/* ---- files by name -------------------------------------------------- */

int remove(const char* path) {
    if (mos_exists(path) <= 0) { errno = ENOENT; return -1; }
    long r = mos_is_dir(path) > 0 ? mos_rmdir(path) : mos_delete(path);
    if (r != 0) { errno = EIO; return -1; }
    return 0;
}

int rename(const char* from, const char* to) {
    long r = mos_rename(from, to);
    if (r != 0) { errno = r == (long)SYS_ERR_NOTFOUND ? ENOENT : EIO; return -1; }
    return 0;
}
