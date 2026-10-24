/*
 * Minimal stdio over uwph fds, plus the few libc helpers whose results depend
 * on Linux numbering (strerror). Wine's Unix side uses FILE for config and
 * /proc-style reads (fopen/fgets/getline/fscanf) and for debug output
 * (fprintf(stderr), vfprintf). Formatting itself comes from the CRT
 * (vsnprintf); only the I/O goes through uwph.
 */
#ifndef UWPH_STDIO_H
#define UWPH_STDIO_H
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct uwph_FILE uwph_FILE;
extern uwph_FILE *uwph_stdout;
extern uwph_FILE *uwph_stderr;
extern char **uwph_environ;

UWPH_API uwph_FILE *uwph_fopen(const char *path, const char *mode);
UWPH_API uwph_FILE *uwph_fdopen(int fd, const char *mode);
UWPH_API int    uwph_fclose(uwph_FILE *f);
UWPH_API size_t uwph_fread(void *buf, size_t sz, size_t n, uwph_FILE *f);
UWPH_API size_t uwph_fwrite(const void *buf, size_t sz, size_t n, uwph_FILE *f);
UWPH_API char  *uwph_fgets(char *s, int n, uwph_FILE *f);
UWPH_API int    uwph_fputs(const char *s, uwph_FILE *f);
UWPH_API int    uwph_fputc(int c, uwph_FILE *f);
UWPH_API int    uwph_puts(const char *s);
UWPH_API int    uwph_feof(uwph_FILE *f);
UWPH_API int    uwph_fileno(uwph_FILE *f);
UWPH_API int    uwph_fflush(uwph_FILE *f);
UWPH_API void   uwph_setbuf(uwph_FILE *f, char *buf);
UWPH_API int    uwph_setvbuf(uwph_FILE *f, char *buf, int mode, size_t size);
UWPH_API int    uwph_fprintf(uwph_FILE *f, const char *fmt, ...);
UWPH_API int    uwph_vfprintf(uwph_FILE *f, const char *fmt, va_list ap);
UWPH_API uwph_ssize_t uwph___getdelim(char **line, size_t *n, int delim, uwph_FILE *f);
UWPH_API int    uwph_fscanf(uwph_FILE *f, const char *fmt, ...);
UWPH_API void   uwph_perror(const char *s);
UWPH_API char  *uwph_strerror(int err);           /* Linux errno numbering */

/* misc libc entry points with host meaning */
struct uwph_mntent;
UWPH_API struct uwph_mntent *uwph_getmntent(uwph_FILE *f);
UWPH_API long uwph_times(struct uwph_tms *t);
UWPH_API int  uwph_tcgetattr(int fd, void *termios);
UWPH_API int  uwph_tcsetattr(int fd, int act, const void *termios);
UWPH_API unsigned int uwph_if_nametoindex(const char *name);
UWPH_API int  uwph_posix_fadvise(int fd, uwph_off_t off, uwph_off_t len, int advice);
UWPH_API int  uwph__dl_find_object(void *pc, void *result);

#ifdef __cplusplus
}
#endif
#endif
