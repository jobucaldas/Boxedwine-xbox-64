#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
#include "internal.h"
#include <stdio.h>
uwph_FILE *uwph_stdout, *uwph_stderr;
__attribute__((constructor)) static void stdio_init(void)
{ uwph_stdout = (uwph_FILE *)stdout; uwph_stderr = (uwph_FILE *)stderr; }
uwph_FILE * uwph_fopen(const char *path, const char *mode)
{
    uwph_trace_hit(UWPH_TRACE_FOPEN);
    return uwph_linux_fopen(path, mode);
}
uwph_FILE * uwph_fdopen(int fd, const char *mode)
{
    return uwph_linux_fdopen(fd, mode);
}
int uwph_fclose(uwph_FILE *f)
{
    uwph_trace_hit(UWPH_TRACE_FCLOSE);
    return uwph_linux_fclose(f);
}
int uwph_feof(uwph_FILE *f)
{
    uwph_trace_hit(UWPH_TRACE_FEOF);
    UWPH_NATIVE(int, "feof", (uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(f));
}
size_t uwph_fread(void *p, size_t s, size_t n, uwph_FILE *f)
{
    UWPH_NATIVE(size_t, "fread", (void *p, size_t s, size_t n, uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(p, s, n, f));
}
size_t uwph_fwrite(const void *p, size_t s, size_t n, uwph_FILE *f)
{
    UWPH_NATIVE(size_t, "fwrite", (const void *p, size_t s, size_t n, uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(p, s, n, f));
}
char * uwph_fgets(char *p, int n, uwph_FILE *f)
{
    UWPH_NATIVE(char *, "fgets", (char *p, int n, uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(p, n, f));
}
int uwph_fputs(const char *p, uwph_FILE *f)
{
    UWPH_NATIVE(int, "fputs", (const char *p, uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(p, f));
}
int uwph_fputc(int c, uwph_FILE *f)
{
    UWPH_NATIVE(int, "fputc", (int c, uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(c, f));
}
int uwph_puts(const char *p)
{
    UWPH_NATIVE(int, "puts", (const char *p));
    UWPH_NATIVE_RETURN(fn(p));
}
int uwph_fileno(uwph_FILE *f)
{
    return uwph_linux_fileno(f);
}
int uwph_fflush(uwph_FILE *f)
{
    UWPH_NATIVE(int, "fflush", (uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(f));
}
int uwph_setvbuf(uwph_FILE *f, char *p, int mode, size_t n)
{
    UWPH_NATIVE(int, "setvbuf", (uwph_FILE *f, char *p, int mode, size_t n));
    UWPH_NATIVE_RETURN(fn(f, p, mode, n));
}
uwph_ssize_t uwph___getdelim(char **p, size_t *n, int d, uwph_FILE *f)
{
    UWPH_NATIVE(uwph_ssize_t, "__getdelim", (char **p, size_t *n, int d, uwph_FILE *f));
    UWPH_NATIVE_RETURN(fn(p, n, d, f));
}
int uwph_vfprintf(uwph_FILE *f, const char *fmt, va_list ap)
{
    UWPH_NATIVE(int, "vfprintf", (uwph_FILE *f, const char *fmt, va_list ap));
    UWPH_NATIVE_RETURN(fn(f, fmt, ap));
}
int uwph_vfscanf(uwph_FILE *f, const char *fmt, va_list ap)
{
    uwph_trace_hit(UWPH_TRACE___ISOC23_FSCANF);
    UWPH_NATIVE(int, "__isoc23_vfscanf", (uwph_FILE *f, const char *fmt, va_list ap));
    UWPH_NATIVE_RETURN(fn(f, fmt, ap));
}
void uwph_setbuf(uwph_FILE *f, char *p)
{ UWPH_NATIVE(void, "setbuf", (uwph_FILE *, char *)); fn(f, p); }
int uwph_fprintf(uwph_FILE *f, const char *fmt, ...)
{ va_list ap; int n; va_start(ap, fmt); n = uwph_vfprintf(f, fmt, ap); va_end(ap); return n; }
int uwph_fscanf(uwph_FILE *f, const char *fmt, ...)
{ va_list ap; int n; va_start(ap, fmt); n = uwph_vfscanf(f, fmt, ap); va_end(ap); return n; }
#endif
