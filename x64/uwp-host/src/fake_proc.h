/* Minimal in-process proc snapshots. No host /proc reads. */
#ifndef UWPH_FAKE_PROC_H
#define UWPH_FAKE_PROC_H
#include <stddef.h>
/* 1: allocated snapshot, 0: not a proc path, -1: unsupported/error. */
int uwph_fake_proc_snapshot(const char *path, char **data, size_t *length);
int uwph_fake_proc_exe(char *buf, size_t size);
int uwph_fake_proc_dir(const char *path);
int uwph_fake_dir_owned(const void *dir);
int uwph_fake_proc_link(const char *path, char *buf, size_t size);
void uwph_proc_fd_open(int fd, const char *path);
void uwph_proc_fd_close(int fd);
void uwph_proc_mapping(void *addr, size_t len, int mapped);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
int uwph_proc_pthread_create(void *out, const void *attr, void *(*start)(void *), void *arg);
#endif

uwph_DIR *uwph_fake_opendir(const char *path);
struct uwph_dirent *uwph_fake_readdir(uwph_DIR *dir);
int uwph_fake_closedir(uwph_DIR *dir);

#endif
