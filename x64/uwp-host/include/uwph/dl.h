/*
 * dlopen/dlsym for Wine's Unix libraries.
 *
 * On Linux Wine dlopen()s ntdll.so, win32u.so, winevulkan.so, ws2_32.so ...
 * and looks up __wine_unix_call_funcs. In the UWP app every Unix library is
 * linked into the signed app binary at build time; dlopen() only finds
 * entries in this static registry, so no host code is ever loaded at
 * runtime (guest PE images are a separate matter: Wine's own PE loader maps
 * them through mmap + codeGeneration). dladdr/_dl_find_object answer from
 * the same registry (Wine uses them for unwinding and module lookup).
 */
#ifndef UWPH_DL_H
#define UWPH_DL_H
#ifdef __cplusplus
extern "C" {
#endif

struct uwph_dl_symbol { const char *name; void *addr; };
struct uwph_dl_library {
    const char *soname;                       /* e.g. "ntdll.so" */
    const struct uwph_dl_symbol *symbols;     /* NULL-name terminated */
    void *text_start, *text_end;              /* for dladdr */
};

int uwph_dl_register(const struct uwph_dl_library *lib);

typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } uwph_Dl_info;

UWPH_API void *uwph_dlopen(const char *file, int mode);
UWPH_API void *uwph_dlsym(void *handle, const char *name);
UWPH_API int   uwph_dlclose(void *handle);
UWPH_API char *uwph_dlerror(void);
UWPH_API int   uwph_dladdr(const void *addr, uwph_Dl_info *info);

#ifdef __cplusplus
}
#endif
#endif
