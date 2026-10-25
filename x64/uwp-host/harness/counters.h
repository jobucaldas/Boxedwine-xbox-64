#ifndef HARNESS_COUNTERS_H
#define HARNESS_COUNTERS_H
enum { C_OPEN, C_READ, C_CLOSE, C_MMAP, C_MUNMAP, C_PROTECT, C_COUNT };
int count_begin(int op, void *caller);
void count_end(void);
void count_bypass(int op);
void uwph_harness_backend_entry(int op);
void uwph_shim_report(void);
long harness_raw(long n, long a, long b, long c, long d, long e, long f);
#endif
