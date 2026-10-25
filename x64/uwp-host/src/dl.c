/* Static registry standing in for dlopen of Wine's Unix libraries. */
#include "internal.h"
#include <stdio.h>

#define MAX_LIBS 64
static const struct uwph_dl_library *libs[MAX_LIBS];
static int nlibs;
static __thread char dl_err[160];
static __thread int dl_err_set;

static void set_err(const char *what, const char *name)
{
    snprintf(dl_err, sizeof(dl_err), "%s: %s (uwph: Unix libraries are linked in, not loaded)", name, what);
    dl_err_set = 1;
}

int uwph_dl_register(const struct uwph_dl_library *lib)
{
    if (nlibs == MAX_LIBS) return UWPH_FAIL(UWPH_ENOMEM);
    libs[nlibs++] = lib;
    return 0;
}

UWPH_API void *uwph_dlopen(const char *file, int mode)
{
    const char *base;
    (void)mode;
    if (!file) return (void *)libs;     /* "main program": searches every library */
    base = strrchr(file, '/');
    base = base ? base + 1 : file;
    for (int i = 0; i < nlibs; i++)
        if (!strcmp(libs[i]->soname, base)) return (void *)libs[i];
    set_err("not in the static registry", file);
    return NULL;
}

static void *lookup(const struct uwph_dl_library *lib, const char *name)
{
    for (const struct uwph_dl_symbol *s = lib->symbols; s && s->name; s++)
        if (!strcmp(s->name, name)) return s->addr;
    return NULL;
}

UWPH_API void *uwph_dlsym(void *handle, const char *name)
{
    void *p = NULL;
    if (handle == (void *)libs) {
        for (int i = 0; i < nlibs && !p; i++) p = lookup(libs[i], name);
    } else if (handle) p = lookup(handle, name);
    if (!p) set_err("undefined symbol", name);
    return p;
}

UWPH_API int uwph_dlclose(void *handle) { (void)handle; return 0; }

UWPH_API char *uwph_dlerror(void)
{
    if (!dl_err_set) return NULL;
    dl_err_set = 0;
    return dl_err;
}

UWPH_API int uwph_dladdr(const void *addr, uwph_Dl_info *info)
{
    uwph_trace_hit(UWPH_TRACE_DLADDR);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "dladdr", (const void *, uwph_Dl_info *));
    UWPH_NATIVE_RETURN(fn(addr, info));
#endif

    for (int i = 0; i < nlibs; i++) {
        const struct uwph_dl_library *l = libs[i];
        if ((const char *)addr < (const char *)l->text_start || (const char *)addr >= (const char *)l->text_end) continue;
        info->dli_fname = l->soname;
        info->dli_fbase = l->text_start;
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
        for (const struct uwph_dl_symbol *s = l->symbols; s && s->name; s++)
            if ((const char *)s->addr <= (const char *)addr &&
                (!info->dli_saddr || (const char *)s->addr > (const char *)info->dli_saddr)) {
                info->dli_sname = s->name;
                info->dli_saddr = s->addr;
            }
        return 1;
    }
    return 0;
}
