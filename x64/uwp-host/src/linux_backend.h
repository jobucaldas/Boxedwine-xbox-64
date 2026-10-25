/* Harness-only adapters; public types come from uwph.h. */
int uwph_linux_open(const char *path, int flags, unsigned mode);
int uwph_linux_openat(int dirfd, const char *path, int flags, unsigned mode);
int uwph_linux_stat(const char *path, struct uwph_stat *st);
int uwph_linux_lstat(const char *path, struct uwph_stat *st);
int uwph_linux_fstat(int fd, struct uwph_stat *st);
int uwph_linux_fstatat(int dirfd, const char *path, struct uwph_stat *st, int flags);
uwph_ssize_t uwph_linux_read(int fd, void *buf, size_t len);
uwph_ssize_t uwph_linux_write(int fd, const void *buf, size_t len);
int uwph_linux_close(int fd);
uwph_ssize_t uwph_linux_pread(int fd, void *buf, size_t len, uwph_off_t off);
uwph_ssize_t uwph_linux_pwrite(int fd, const void *buf, size_t len, uwph_off_t off);
uwph_off_t uwph_linux_lseek(int fd, uwph_off_t off, int whence);
void * uwph_linux_mmap(void *addr, size_t len, int prot, int flags, int fd, uwph_off_t off);
int uwph_linux_munmap(void *addr, size_t len);
int uwph_linux_mprotect(void *addr, size_t len, int prot);
int uwph_linux_clock_gettime(uwph_clockid_t id, struct uwph_timespec *ts);
int uwph_linux_clock_getres(uwph_clockid_t id, struct uwph_timespec *ts);
int uwph_linux_nanosleep(const struct uwph_timespec *req, struct uwph_timespec *rem);
int uwph_linux_gettid(void);
long uwph_linux_futex(int *uaddr, int op, int val, const struct uwph_timespec *timeout,
                         int *uaddr2, int val3);

void *uwph_linux_symbol(const char *name);
int uwph_linux_fcntl(int fd, int cmd, long arg);
uwph_ssize_t uwph_linux_writev(int fd, const struct uwph_iovec *iov, int cnt);

/* Native FILE bridges share private descriptor ownership with fd operations. */
uwph_FILE *uwph_linux_fopen(const char *path, const char *mode);
uwph_FILE *uwph_linux_fdopen(int fd, const char *mode);
int uwph_linux_fileno(uwph_FILE *f);
int uwph_linux_fclose(uwph_FILE *f);

int uwph_linux_isatty(int fd);
int uwph_linux_pipe2(int fds[2], int flags);

/* Explicit descriptor domain: Wine borrows native fds; direct tests do not. */
int uwph_linux_native_fds(void);

int uwph_linux_ftruncate(int fd, uwph_off_t len);
int uwph_linux_dup(int fd);
int uwph_linux_fchdir(int fd);
