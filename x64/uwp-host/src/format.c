/* Formatting and locale entries Wine's Unix side calls; counted per O2 trace. */
#include "internal.h"
#include <stdio.h>
#include <locale.h>
#include <errno.h>

UWPH_API int uwph_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int n;
    uwph_trace_hit(UWPH_TRACE_SNPRINTF);
    n = vsnprintf(buf, size, fmt, ap);
    if (n < 0) uwph_errno = UWPH_EINVAL;
    return n;
}
UWPH_API int uwph_snprintf(char *buf, size_t size, const char *fmt, ...)
{ va_list ap; int n; va_start(ap, fmt); n=uwph_vsnprintf(buf,size,fmt,ap); va_end(ap); return n; }
UWPH_API int uwph_vasprintf(char **buf, const char *fmt, va_list ap)
{
    va_list cp;
    int n;
    uwph_trace_hit(UWPH_TRACE_ASPRINTF);
    *buf = NULL;
    va_copy(cp, ap);
    n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n >= 0 && (*buf = malloc((size_t)n + 1))) vsnprintf(*buf, (size_t)n + 1, fmt, ap);
    if (!*buf) return UWPH_FAIL(n < 0 ? UWPH_EINVAL : UWPH_ENOMEM);
    return n;
}
UWPH_API int uwph_asprintf(char **buf, const char *fmt, ...)
{ va_list ap; int n; va_start(ap, fmt); n=uwph_vasprintf(buf,fmt,ap); va_end(ap); return n; }
UWPH_API char *uwph_setlocale(int category, const char *locale)
{
    /* Indexed by UWPH_LC_* (Linux numbering); host constants differ on the CRT.
     * The CRT has no LC_MESSAGES: only the C/POSIX locale is accepted there. */
    static const int categories[] = {
        [UWPH_LC_CTYPE] = LC_CTYPE, [UWPH_LC_NUMERIC] = LC_NUMERIC, [UWPH_LC_TIME] = LC_TIME,
        [UWPH_LC_COLLATE] = LC_COLLATE, [UWPH_LC_MONETARY] = LC_MONETARY,
#ifdef LC_MESSAGES
        [UWPH_LC_MESSAGES] = LC_MESSAGES,
#else
        [UWPH_LC_MESSAGES] = -1,
#endif
        [UWPH_LC_ALL] = LC_ALL,
    };
    char *r;
    uwph_trace_hit(UWPH_TRACE_SETLOCALE);
    if ((unsigned)category >= sizeof(categories) / sizeof(categories[0])) return UWPH_FAILP(UWPH_EINVAL);
    if (categories[category] < 0) {
        if (!locale || !*locale || !strcmp(locale, "C") || !strcmp(locale, "POSIX")) return "C";
        return UWPH_FAILP(UWPH_EINVAL);
    }
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(char *, "setlocale", (int, const char *));
    r = fn(categories[category], locale);
#else
    r = setlocale(categories[category], locale);
#endif
    if (!r) uwph_errno = UWPH_EINVAL;
    return r;
}
