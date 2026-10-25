/* Minimal in-process proc snapshots. No host /proc reads. */
#ifndef UWPH_FAKE_PROC_H
#define UWPH_FAKE_PROC_H
#include <stddef.h>
/* 1: allocated snapshot, 0: not a proc path, -1: unsupported/error. */
int uwph_fake_proc_snapshot(const char *path, char **data, size_t *length);
int uwph_fake_proc_exe(char *buf, size_t size);
#endif
