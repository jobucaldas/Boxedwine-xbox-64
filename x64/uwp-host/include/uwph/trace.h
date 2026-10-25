/* Per-entry counters; no logging or host calls on the counted path. */
#ifndef UWPH_TRACE_H
#define UWPH_TRACE_H
#ifdef __cplusplus
extern "C" {
#endif
enum uwph_trace_symbol {
    UWPH_TRACE_SNPRINTF,
    UWPH_TRACE_FOPEN,
    UWPH_TRACE___ISOC23_FSCANF,
    UWPH_TRACE_FCLOSE,
    UWPH_TRACE_PTHREAD_GETSPECIFIC,
    UWPH_TRACE_PTHREAD_SIGMASK,
    UWPH_TRACE_FEOF,
    UWPH_TRACE_STAT,
    UWPH_TRACE_WRITE,
    UWPH_TRACE_PTHREAD_MUTEX_LOCK,
    UWPH_TRACE_PTHREAD_MUTEX_UNLOCK,
    UWPH_TRACE_WRITEV,
    UWPH_TRACE_GETENV,
    UWPH_TRACE_ASPRINTF,
    UWPH_TRACE_SIGADDSET,
    UWPH_TRACE_FCNTL,
    UWPH_TRACE_SETLOCALE,
    UWPH_TRACE_TIME,
    UWPH_TRACE_GETUID,
    UWPH_TRACE_REALPATH,
    UWPH_TRACE_COUNT
};
UWPH_API uint64_t uwph_trace_count(enum uwph_trace_symbol symbol);
UWPH_API const char *uwph_trace_name(enum uwph_trace_symbol symbol);
#ifdef __cplusplus
}
#endif
#endif
