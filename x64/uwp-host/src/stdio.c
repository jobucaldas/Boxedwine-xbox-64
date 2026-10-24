/* Minimal buffered stdio over uwph fds. */
#include "internal.h"
#include <stdio.h>

struct uwph_FILE {
    int fd;
    int eof, err;
    int rpos, rlen;
    int unbuffered;
    char rbuf[4096];
};

static uwph_FILE f_stdout = { 1, 0, 0, 0, 0, 1, {0} };
static uwph_FILE f_stderr = { 2, 0, 0, 0, 0, 1, {0} };
uwph_FILE *uwph_stdout = &f_stdout;
uwph_FILE *uwph_stderr = &f_stderr;

static int mode_flags(const char *mode)
{
    int fl = mode[0] == 'r' ? UWPH_O_RDONLY : UWPH_O_WRONLY | UWPH_O_CREAT | (mode[0] == 'a' ? UWPH_O_APPEND : UWPH_O_TRUNC);
    if (strchr(mode, '+')) fl = (fl & ~UWPH_O_ACCMODE) | UWPH_O_RDWR;
    if (strchr(mode, 'e')) fl |= UWPH_O_CLOEXEC;
    return fl;
}

UWPH_API uwph_FILE *uwph_fdopen(int fd, const char *mode)
{
    uwph_FILE *f;
    (void)mode;
    if (!(f = calloc(1, sizeof(*f)))) return UWPH_FAILP(UWPH_ENOMEM);
    f->fd = fd;
    return f;
}

UWPH_API uwph_FILE *uwph_fopen(const char *path, const char *mode)
{
    int fd = uwph_open(path, mode_flags(mode), 0666);
    uwph_FILE *f;
    if (fd < 0) return NULL;
    if (!(f = uwph_fdopen(fd, mode))) uwph_close(fd);
    return f;
}

UWPH_API int uwph_fclose(uwph_FILE *f)
{
    int r;
    if (f == uwph_stdout || f == uwph_stderr) return 0;
    r = uwph_close(f->fd);
    free(f);
    return r;
}

static int fill(uwph_FILE *f)
{
    uwph_ssize_t n;
    if (f->rpos < f->rlen) return 1;
    n = uwph_read(f->fd, f->rbuf, sizeof(f->rbuf));
    if (n <= 0) { if (n < 0) f->err = 1; else f->eof = 1; return 0; }
    f->rpos = 0;
    f->rlen = (int)n;
    return 1;
}

UWPH_API size_t uwph_fread(void *buf, size_t sz, size_t n, uwph_FILE *f)
{
    size_t want = sz * n, got = 0;
    if (!want) return 0;
    while (got < want && fill(f)) {
        size_t c = (size_t)(f->rlen - f->rpos);
        if (c > want - got) c = want - got;
        memcpy((char *)buf + got, f->rbuf + f->rpos, c);
        f->rpos += (int)c;
        got += c;
    }
    return got / sz;
}

UWPH_API size_t uwph_fwrite(const void *buf, size_t sz, size_t n, uwph_FILE *f)
{
    size_t want = sz * n, done = 0;
    while (done < want) {
        uwph_ssize_t w = uwph_write(f->fd, (const char *)buf + done, want - done);
        if (w <= 0) { f->err = 1; break; }
        done += (size_t)w;
    }
    return sz ? done / sz : 0;
}

UWPH_API char *uwph_fgets(char *s, int n, uwph_FILE *f)
{
    int i = 0;
    if (n <= 0) return NULL;
    while (i < n - 1 && fill(f)) {
        char c = f->rbuf[f->rpos++];
        s[i++] = c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}

UWPH_API uwph_ssize_t uwph___getdelim(char **line, size_t *n, int delim, uwph_FILE *f)
{
    size_t i = 0;
    if (!*line || !*n) { *n = 128; if (!(*line = malloc(*n))) return UWPH_FAIL(UWPH_ENOMEM); }
    while (fill(f)) {
        char c = f->rbuf[f->rpos++];
        if (i + 2 > *n) {
            char *nl = realloc(*line, *n * 2);
            if (!nl) return UWPH_FAIL(UWPH_ENOMEM);
            *line = nl;
            *n *= 2;
        }
        (*line)[i++] = c;
        if (c == delim) break;
    }
    if (!i) return -1;
    (*line)[i] = 0;
    return (uwph_ssize_t)i;
}

UWPH_API int uwph_fputs(const char *s, uwph_FILE *f)
{
    size_t l = strlen(s);
    return uwph_fwrite(s, 1, l, f) == l ? 0 : -1;
}

UWPH_API int uwph_fputc(int c, uwph_FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return uwph_fwrite(&ch, 1, 1, f) == 1 ? c : -1;
}

UWPH_API int uwph_puts(const char *s)
{
    if (uwph_fputs(s, uwph_stdout) < 0) return -1;
    return uwph_fputc('\n', uwph_stdout) < 0 ? -1 : 1;
}

UWPH_API int uwph_feof(uwph_FILE *f) { return f->eof; }
UWPH_API int uwph_fileno(uwph_FILE *f) { return f->fd; }
UWPH_API int uwph_fflush(uwph_FILE *f) { (void)f; return 0; }   /* writes are unbuffered */
UWPH_API void uwph_setbuf(uwph_FILE *f, char *buf) { (void)buf; f->unbuffered = 1; }
UWPH_API int uwph_setvbuf(uwph_FILE *f, char *buf, int mode, size_t size)
{ (void)buf; (void)mode; (void)size; f->unbuffered = 1; return 0; }

UWPH_API int uwph_vfprintf(uwph_FILE *f, const char *fmt, va_list ap)
{
    char small[1024], *p = small;
    va_list cp;
    int n;
    va_copy(cp, ap);
    n = vsnprintf(small, sizeof(small), fmt, cp);
    va_end(cp);
    if (n < 0) return n;
    if ((size_t)n >= sizeof(small)) {
        if (!(p = malloc((size_t)n + 1))) return UWPH_FAIL(UWPH_ENOMEM);
        vsnprintf(p, (size_t)n + 1, fmt, ap);
    }
    n = uwph_fwrite(p, 1, (size_t)n, f) == (size_t)n ? n : -1;
    if (p != small) free(p);
    return n;
}

UWPH_API int uwph_fprintf(uwph_FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = uwph_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

UWPH_API int uwph_fscanf(uwph_FILE *f, const char *fmt, ...)
{
    /* Wine scans /proc and /sys files with it; those do not exist here */
    (void)f; (void)fmt;
    return UWPH_ENOSYS_INT();
}

static const char *const errstr[] = {
    [0] = "Success", [UWPH_EPERM] = "Operation not permitted", [UWPH_ENOENT] = "No such file or directory",
    [UWPH_ESRCH] = "No such process", [UWPH_EINTR] = "Interrupted system call", [UWPH_EIO] = "Input/output error",
    [UWPH_EBADF] = "Bad file descriptor", [UWPH_ECHILD] = "No child processes",
    [UWPH_EAGAIN] = "Resource temporarily unavailable", [UWPH_ENOMEM] = "Cannot allocate memory",
    [UWPH_EACCES] = "Permission denied", [UWPH_EFAULT] = "Bad address", [UWPH_EEXIST] = "File exists",
    [UWPH_ENOTDIR] = "Not a directory", [UWPH_EISDIR] = "Is a directory", [UWPH_EINVAL] = "Invalid argument",
    [UWPH_EMFILE] = "Too many open files", [UWPH_ENOTTY] = "Inappropriate ioctl for device",
    [UWPH_ESPIPE] = "Illegal seek", [UWPH_EROFS] = "Read-only file system", [UWPH_EPIPE] = "Broken pipe",
    [UWPH_ENOSYS] = "Function not implemented (uwph)", [UWPH_ENOTSOCK] = "Socket operation on non-socket",
    [UWPH_EAFNOSUPPORT] = "Address family not supported by protocol", [UWPH_EADDRINUSE] = "Address already in use",
    [UWPH_ENOTCONN] = "Transport endpoint is not connected", [UWPH_ETIMEDOUT] = "Connection timed out",
    [UWPH_ECONNREFUSED] = "Connection refused",
};

UWPH_API char *uwph_strerror(int err)
{
    static __thread char buf[32];
    if (err >= 0 && err < (int)(sizeof(errstr) / sizeof(errstr[0])) && errstr[err]) return (char *)errstr[err];
    snprintf(buf, sizeof(buf), "Unknown error %d", err);
    return buf;
}

UWPH_API void uwph_perror(const char *s)
{
    if (s && *s) uwph_fprintf(uwph_stderr, "%s: %s\n", s, uwph_strerror(uwph_errno));
    else uwph_fprintf(uwph_stderr, "%s\n", uwph_strerror(uwph_errno));
}

/* ---- misc ---- */
UWPH_API struct uwph_mntent *uwph_getmntent(uwph_FILE *f) { (void)f; return NULL; }  /* no mount table */
UWPH_API long uwph_times(struct uwph_tms *t) { (void)t; return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_tcgetattr(int fd, void *t) { (void)fd; (void)t; return UWPH_FAIL(UWPH_ENOTTY); }
UWPH_API int uwph_tcsetattr(int fd, int a, const void *t) { (void)fd; (void)a; (void)t; return UWPH_FAIL(UWPH_ENOTTY); }
UWPH_API unsigned int uwph_if_nametoindex(const char *name) { (void)name; uwph_errno = UWPH_ENOSYS; return 0; }
UWPH_API int uwph_posix_fadvise(int fd, uwph_off_t off, uwph_off_t len, int advice)
{ (void)fd; (void)off; (void)len; (void)advice; return 0; }
UWPH_API int uwph__dl_find_object(void *pc, void *result) { (void)pc; (void)result; return -1; }  /* -> dladdr registry */
