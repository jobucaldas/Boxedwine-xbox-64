/*
 * Runtime capability probe. Several Win32 APIs the host would like to use are
 * documented "desktop apps only" (AddVectoredExceptionHandler,
 * SetThreadContext, RtlAddGrowableFunctionTable, RtlInstallFunctionTableCallback,
 * VirtualAlloc2FromApp, MapViewOfFile3FromApp). Whether a Dev Mode app on the
 * console can call them is a hardware question, so the host resolves them at
 * startup and picks a strategy; the report is the first thing to run on the
 * Xbox.
 */
#ifndef UWPH_PROBE_H
#define UWPH_PROBE_H
#ifdef __cplusplus
extern "C" {
#endif

enum uwph_cap {
    UWPH_CAP_VIRTUALALLOC_FROMAPP,
    UWPH_CAP_VIRTUALPROTECT_FROMAPP,
    UWPH_CAP_VIRTUALALLOC2_FROMAPP,
    UWPH_CAP_MAPVIEW_FROMAPP,
    UWPH_CAP_MAPVIEW3_FROMAPP,
    UWPH_CAP_CODEGEN_EXEC,          /* RW -> RX flip actually works */
    UWPH_CAP_VEH,                   /* AddVectoredExceptionHandler resolvable */
    UWPH_CAP_SET_THREAD_CONTEXT,
    UWPH_CAP_FUNCTION_TABLE_CALLBACK,
    UWPH_CAP_WAIT_ON_ADDRESS,
    UWPH_CAP_COUNT
};

int  uwph_probe_run(void);                 /* fills the table, returns #caps present */
int  uwph_probe_has(enum uwph_cap cap);
const char *uwph_probe_name(enum uwph_cap cap);
void uwph_probe_print(void (*out)(const char *line));

#ifdef __cplusplus
}
#endif
#endif
