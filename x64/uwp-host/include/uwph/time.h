/* Clocks and sleeps: QueryPerformanceCounter / GetSystemTimePreciseAsFileTime /
 * QueryUnbiasedInterruptTime; sleeps on WaitOnAddress so they are
 * interruptible by queued signals. */
#ifndef UWPH_TIME_H
#define UWPH_TIME_H
#ifdef __cplusplus
extern "C" {
#endif

UWPH_API int uwph_clock_gettime(uwph_clockid_t id, struct uwph_timespec *ts);
UWPH_API int uwph_clock_getres(uwph_clockid_t id, struct uwph_timespec *ts);
UWPH_API int uwph_gettimeofday(struct uwph_timeval *tv, void *tz);
UWPH_API uwph_time_t uwph_time(uwph_time_t *t);
UWPH_API int uwph_nanosleep(const struct uwph_timespec *req, struct uwph_timespec *rem);
UWPH_API int uwph_usleep(uint32_t usec);

#ifdef __cplusplus
}
#endif
#endif
