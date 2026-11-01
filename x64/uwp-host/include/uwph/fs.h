/*
 * Filesystem. Wine's Unix side sees a POSIX tree; uwph maps it onto the app's
 * own storage only:
 *     /            -> ApplicationData LocalFolder (read-write; prefix, dosdevices,
 *                     wineserver dir, user's game installs)
 *     /app         -> Package InstallLocation (read-only; Wine binaries, DLLs)
 * Paths outside the mount table fail with ENOENT. Host calls: CreateFile2,
 * GetFileInformationByHandleEx, FindFirstFileExW, SetFileInformationByHandle.
 *
 * Wine-specific needs: DOS attributes in the "user.DOSATTRIB" xattr
 * (-> NTFS file attributes), case-insensitive lookup is done by Wine itself,
 * symlinks for dosdevices (c: -> ../drive_c) are emulated by a small link
 * table because CreateSymbolicLinkW is not in the APP partition.
 */
#ifndef UWPH_FS_H
#define UWPH_FS_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct uwph_dir uwph_DIR;

UWPH_API int uwph_open(const char *path, int flags, ...);
UWPH_API int uwph_openat(int dirfd, const char *path, int flags, ...);
UWPH_API int uwph_creat(const char *path, uwph_mode_t mode);
UWPH_API int uwph_stat(const char *path, struct uwph_stat *st);
UWPH_API int uwph_lstat(const char *path, struct uwph_stat *st);
UWPH_API int uwph_fstat(int fd, struct uwph_stat *st);
UWPH_API int uwph_fstatat(int dirfd, const char *path, struct uwph_stat *st, int flags);
UWPH_API int uwph_fstatfs(int fd, struct uwph_statfs *st);
UWPH_API int uwph_access(const char *path, int mode);
UWPH_API int uwph_mkdir(const char *path, uwph_mode_t mode);
UWPH_API int uwph_rmdir(const char *path);
UWPH_API int uwph_unlink(const char *path);
UWPH_API int uwph_remove(const char *path);
UWPH_API int uwph_rename(const char *from, const char *to);
UWPH_API int uwph_link(const char *from, const char *to);
UWPH_API int uwph_symlink(const char *target, const char *path);
UWPH_API uwph_ssize_t uwph_readlink(const char *path, char *buf, size_t len);
UWPH_API char *uwph_realpath(const char *path, char *resolved);
UWPH_API char *uwph_getcwd(char *buf, size_t len);
UWPH_API int uwph_chdir(const char *path);
UWPH_API int uwph_fchdir(int fd);
UWPH_API int uwph_fchmod(int fd, uwph_mode_t mode);
UWPH_API int uwph_chmod(const char *path, uwph_mode_t mode);
UWPH_API uwph_mode_t uwph_umask(uwph_mode_t mask);
UWPH_API int uwph_fsync(int fd);
UWPH_API int uwph_ftruncate(int fd, uwph_off_t len);
UWPH_API int uwph_posix_fallocate(int fd, uwph_off_t off, uwph_off_t len);
UWPH_API int uwph_futimens(int fd, const struct uwph_timespec times[2]);
UWPH_API int uwph_utimensat(int dirfd, const char *path, const struct uwph_timespec times[2], int flags);
UWPH_API char *uwph_mkdtemp(char *tmpl);
UWPH_API uwph_DIR *uwph_opendir(const char *path);
UWPH_API uwph_DIR *uwph_fdopendir(int fd);
UWPH_API struct uwph_dirent *uwph_readdir(uwph_DIR *d);
UWPH_API int uwph_closedir(uwph_DIR *d);
UWPH_API uwph_ssize_t uwph_getxattr(const char *path, const char *name, void *val, size_t len);
UWPH_API uwph_ssize_t uwph_fgetxattr(int fd, const char *name, void *val, size_t len);
UWPH_API int uwph_fsetxattr(int fd, const char *name, const void *val, size_t len, int flags);
UWPH_API int uwph_fremovexattr(int fd, const char *name);
/* inotify (wineserver change notifications) */
UWPH_API int uwph_inotify_init(void);
UWPH_API int uwph_inotify_add_watch(int fd, const char *path, uint32_t mask);
UWPH_API int uwph_inotify_rm_watch(int fd, int wd);

/* ---- mount table / path translation (portable) ---- */
#define UWPH_MAX_MOUNTS 8
#define UWPH_MOUNT_RO 1
int uwph_fs_mount(const char *unix_prefix, const char *host_dir, int flags);
/* Translate an absolute or cwd-relative Unix path to a host path (UTF-8,
 * '\\' separators on Windows). Rejects ".." escaping a mount. Returns mount
 * flags (>=0) or -errno. */
int uwph_fs_translate(const char *unix_path, char *host, size_t len);

#ifdef __cplusplus
}
#endif
#endif
