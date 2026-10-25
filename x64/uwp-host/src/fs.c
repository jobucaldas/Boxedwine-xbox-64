/*
 * Filesystem: mount table + path translation (portable), and the host file
 * backend. Win32 backend implements read-only regular files through
 * CreateFile2 so far; everything that writes or enumerates is ENOSYS.
 */
#include "internal.h"
#include <stdarg.h>
#include <stdio.h>
#include "fake_proc.h"

struct mount { char prefix[64]; char host[512]; int flags; };
static struct mount mounts[UWPH_MAX_MOUNTS];
static int nmounts;
static char cwd[512] = "/";
static uwph_lock_t fs_lock = UWPH_LOCK_INIT;

int uwph_fs_mount(const char *unix_prefix, const char *host_dir, int flags)
{
    struct mount *m;
    if (nmounts == UWPH_MAX_MOUNTS || strlen(unix_prefix) >= sizeof(m->prefix) ||
        strlen(host_dir) >= sizeof(m->host) || unix_prefix[0] != '/')
        return UWPH_FAIL(UWPH_EINVAL);
    uwph_lock(&fs_lock);
    m = &mounts[nmounts++];
    strcpy(m->prefix, unix_prefix);
    strcpy(m->host, host_dir);
    m->flags = flags;
    uwph_unlock(&fs_lock);
    return 0;
}

/* Normalise against cwd: collapse "//", ".", ".." (clamped at "/"). */
int uwph_fs_abspath(const char *path, char *out, size_t len)
{
    char tmp[1024];
    size_t o = 0;
    const char *p;

    if (!path || !path[0]) return UWPH_FAIL(UWPH_ENOENT);
    if (path[0] == '/') snprintf(tmp, sizeof(tmp), "%s", path);
    else {
        uwph_lock(&fs_lock);
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
        uwph_unlock(&fs_lock);
    }
    if (len < 2) return UWPH_FAIL(UWPH_ENAMETOOLONG);
    out[o++] = '/';
    for (p = tmp; *p;) {
        const char *s;
        size_t n;
        while (*p == '/') p++;
        if (!*p) break;
        for (s = p; *p && *p != '/'; p++);
        n = (size_t)(p - s);
        if (n == 1 && s[0] == '.') continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; }
            continue;
        }
        if (o + n + 2 > len) return UWPH_FAIL(UWPH_ENAMETOOLONG);
        if (o > 1 && out[o - 1] != '/') out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return 0;
}

int uwph_fs_translate(const char *unix_path, char *host, size_t len)
{
    char abs[1024];
    const struct mount *best = NULL;
    size_t bestlen = 0;
    const char *rest;
    int flags;

    if (uwph_fs_abspath(unix_path, abs, sizeof(abs)) < 0) return -uwph_errno;
    uwph_lock(&fs_lock);
    for (int i = 0; i < nmounts; i++) {
        size_t pl = strlen(mounts[i].prefix);
        int match = !strcmp(mounts[i].prefix, "/") ||
                    (!strncmp(abs, mounts[i].prefix, pl) && (abs[pl] == '/' || !abs[pl]));
        if (match && (pl > bestlen || !best)) { best = &mounts[i]; bestlen = strcmp(best->prefix, "/") ? pl : 0; }
    }
    if (!best) { uwph_unlock(&fs_lock); return -UWPH_ENOENT; }
    rest = abs + bestlen;
    if (snprintf(host, len, "%s%s", best->host, rest) >= (int)len) { uwph_unlock(&fs_lock); return -UWPH_ENAMETOOLONG; }
    flags = best->flags;
    uwph_unlock(&fs_lock);
#ifdef UWPH_BACKEND_WIN32
    for (char *c = host; *c; c++) if (*c == '/') *c = '\\';
#endif
    return flags;
}

UWPH_API char *uwph_getcwd(char *buf, size_t len)
{
    uwph_trace_hit(UWPH_TRACE_GETCWD);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(char *, "getcwd", (char *, size_t));
    UWPH_NATIVE_RETURN(fn(buf, len));
#endif
    uwph_lock(&fs_lock);
    if (strlen(cwd) + 1 > len) { uwph_unlock(&fs_lock); return UWPH_FAILP(UWPH_ERANGE); }
    strcpy(buf, cwd);
    uwph_unlock(&fs_lock);
    return buf;
}

UWPH_API int uwph_chdir(const char *path)
{
    uwph_trace_hit(UWPH_TRACE_CHDIR);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "chdir", (const char *));
    UWPH_NATIVE_RETURN(fn(path));
#endif

    char abs[512];
    if (uwph_fs_abspath(path, abs, sizeof(abs)) < 0) return -1;
    /* Virtual cwd also names in-process socket directories. */
    uwph_lock(&fs_lock);
    strcpy(cwd, abs);
    uwph_unlock(&fs_lock);
    return 0;
}

static void fill_socket_stat(struct uwph_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = UWPH_S_IFSOCK | 0700;
    st->st_nlink = 1;
    st->st_uid = st->st_gid = uwph_getuid();
}

/* ---- host file backend ---- */
#ifdef UWPH_BACKEND_WIN32
struct hfile { struct uwph_object base; HANDLE h; char *directory; char *snapshot; size_t snapshot_len; int64_t pos; uwph_lock_t lock; };

static void hfile_destroy(struct uwph_object *o)
{
    struct hfile *f = (struct hfile *)o;
    if (f->directory) free(f->directory);
    else if (f->snapshot) free(f->snapshot); else CloseHandle(f->h);
    free(o);
}

static int utf8_to_wide(const char *s, WCHAR *w, int wlen)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wlen);
}

static int win_errno(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return UWPH_ENOENT;
    case ERROR_ACCESS_DENIED:  return UWPH_EACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return UWPH_EEXIST;
    case ERROR_INVALID_HANDLE: return UWPH_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return UWPH_ENOMEM;
    case ERROR_DIRECTORY: return UWPH_ENOTDIR;
    default: return UWPH_EIO;
    }
}

static int host_open(const char *path, int flags)
{
    char host[1024];
    WCHAR wpath[1024];
    struct hfile *f;
    HANDLE h;
    int mf, fd;

    if ((flags & UWPH_O_ACCMODE) != UWPH_O_RDONLY || (flags & (UWPH_O_CREAT | UWPH_O_TRUNC)))
        return UWPH_ENOSYS_INT();          /* write/create/dirs: next step */
    char *snapshot = NULL; size_t snapshot_len = 0;
    int proc = uwph_fake_proc_snapshot(path, &snapshot, &snapshot_len);
    if (proc < 0 && strcmp(path, "/proc/self/exe")) return -1;
    if (proc > 0) {
        if (!(f = calloc(1, sizeof(*f)))) { free(snapshot); return UWPH_FAIL(UWPH_ENOMEM); }
        uwph_obj_init(&f->base, hfile_destroy);
        f->snapshot = snapshot; f->snapshot_len = snapshot_len;
        InitializeSRWLock(&f->lock);
        if ((fd = uwph_fd_alloc(UWPH_FD_FILE, &f->base, flags)) < 0) uwph_obj_unref(&f->base);
        return fd;
    }
    if (!strcmp(path, "/proc/self/exe")) {
        if (uwph_fake_proc_exe(host, sizeof(host)) < 0) return -1;
        path = host;
    } else
    if ((mf = uwph_fs_translate(path, host, sizeof(host))) < 0) return UWPH_FAIL(-mf);
    if (!utf8_to_wide(host, wpath, 1024)) return UWPH_FAIL(UWPH_ENAMETOOLONG);
    if (flags & UWPH_O_DIRECTORY) {
        DWORD attrs = GetFileAttributesW(wpath);
        char absolute[1024];
        if (attrs == INVALID_FILE_ATTRIBUTES) return UWPH_FAIL(win_errno(GetLastError()));
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return UWPH_FAIL(UWPH_ENOTDIR);
        if (uwph_fs_abspath(path, absolute, sizeof(absolute)) < 0) return -1;
        f = calloc(1, sizeof(*f));
        if (!f) return UWPH_FAIL(UWPH_ENOMEM);
        if (!(f->directory = strdup(absolute))) { free(f); return UWPH_FAIL(UWPH_ENOMEM); }
        uwph_obj_init(&f->base, hfile_destroy);
        InitializeSRWLock(&f->lock);
        fd = uwph_fd_alloc(UWPH_FD_FILE, &f->base, flags);
        if (fd < 0) uwph_obj_unref(&f->base);
        return fd;
    }
    h = CreateFile2(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    OPEN_EXISTING, NULL);
    if (h == INVALID_HANDLE_VALUE) return UWPH_FAIL(win_errno(GetLastError()));
    if (!(f = calloc(1, sizeof(*f)))) { CloseHandle(h); return UWPH_FAIL(UWPH_ENOMEM); }
    uwph_obj_init(&f->base, hfile_destroy);
    f->h = h;
    InitializeSRWLock(&f->lock);
    if ((fd = uwph_fd_alloc(UWPH_FD_FILE, &f->base, flags)) < 0) uwph_obj_unref(&f->base);
    return fd;
}

uwph_ssize_t uwph_file_read(struct uwph_object *o, void *buf, size_t len, int64_t off)
{
    struct hfile *f = (struct hfile *)o;
    OVERLAPPED ov = {0};
    DWORD got = 0;
    int64_t at;
    if (f->directory) return UWPH_FAIL(UWPH_EISDIR);
    if (len > 0x7fffffff) len = 0x7fffffff;
    uwph_lock(&f->lock);
    at = off < 0 ? f->pos : off;
    if (f->snapshot) {
        size_t n = at >= (int64_t)f->snapshot_len ? 0 : f->snapshot_len - (size_t)at;
        if (n > len) n = len;
        if (n) memcpy(buf, f->snapshot + at, n);
        if (off < 0) f->pos += n;
        uwph_unlock(&f->lock); return n;
    }
    ov.Offset = (DWORD)at;
    ov.OffsetHigh = (DWORD)(at >> 32);
    if (!ReadFile(f->h, buf, (DWORD)len, &got, &ov) && GetLastError() != ERROR_HANDLE_EOF) {
        uwph_unlock(&f->lock);
        return UWPH_FAIL(win_errno(GetLastError()));
    }
    if (off < 0) f->pos += got;
    uwph_unlock(&f->lock);
    return got;
}

int64_t uwph_file_seek(struct uwph_object *o, int64_t off, int whence)
{
    struct hfile *f = (struct hfile *)o;
    FILE_STANDARD_INFO si;
    int64_t base = 0, np;
    if (f->directory) return UWPH_FAIL(UWPH_EISDIR);
    uwph_lock(&f->lock);
    if (whence == UWPH_SEEK_CUR) base = f->pos;
    else if (whence == UWPH_SEEK_END) {
        if (f->snapshot) base = f->snapshot_len;
        else if (!GetFileInformationByHandleEx(f->h, FileStandardInfo, &si, sizeof(si))) {
            uwph_unlock(&f->lock);
            return UWPH_FAIL(win_errno(GetLastError()));
        }
        if (!f->snapshot) base = si.EndOfFile.QuadPart;
    } else if (whence != UWPH_SEEK_SET) { uwph_unlock(&f->lock); return UWPH_FAIL(UWPH_EINVAL); }
    if ((np = base + off) < 0) { uwph_unlock(&f->lock); return UWPH_FAIL(UWPH_EINVAL); }
    f->pos = np;
    uwph_unlock(&f->lock);
    return np;
}

static struct uwph_timespec ft_to_ts(LARGE_INTEGER t)
{
    /* 100ns ticks since 1601 -> Unix epoch */
    int64_t v = t.QuadPart - 116444736000000000LL;
    struct uwph_timespec ts = { v / 10000000, (v % 10000000) * 100 };
    return ts;
}

int uwph_file_fstat(struct uwph_object *o, struct uwph_stat *st)
{
    struct hfile *f = (struct hfile *)o;
    FILE_BASIC_INFO bi;
    FILE_STANDARD_INFO si;
    if (f->directory) {
        memset(st, 0, sizeof(*st)); st->st_mode = UWPH_S_IFDIR | 0755; st->st_nlink = 1;
        return 0;
    }
    if (f->snapshot) {
        memset(st, 0, sizeof(*st)); st->st_mode = UWPH_S_IFREG | 0444;
        st->st_nlink = 1; st->st_size = f->snapshot_len; st->st_blksize = 4096;
        return 0;
    }
    if (!GetFileInformationByHandleEx(f->h, FileBasicInfo, &bi, sizeof(bi)) ||
        !GetFileInformationByHandleEx(f->h, FileStandardInfo, &si, sizeof(si)))
        return UWPH_FAIL(win_errno(GetLastError()));
    memset(st, 0, sizeof(*st));
    st->st_mode = (si.Directory ? UWPH_S_IFDIR | 0755 : UWPH_S_IFREG | 0644);
    if (bi.FileAttributes & FILE_ATTRIBUTE_READONLY) st->st_mode &= ~0222u;
    st->st_nlink = si.NumberOfLinks;
    st->st_uid = st->st_gid = uwph_getuid();
    st->st_size = si.EndOfFile.QuadPart;
    st->st_blksize = 4096;
    st->st_blocks = (si.AllocationSize.QuadPart + 511) / 512;
    st->st_atim = ft_to_ts(bi.LastAccessTime);
    st->st_mtim = ft_to_ts(bi.LastWriteTime);
    st->st_ctim = ft_to_ts(bi.ChangeTime);
    return 0;
}
#else
static int host_open(const char *path, int flags) { (void)path; (void)flags; return UWPH_ENOSYS_INT(); }
uwph_ssize_t uwph_file_read(struct uwph_object *o, void *buf, size_t len, int64_t off)
{ (void)o; (void)buf; (void)len; (void)off; return UWPH_ENOSYS_INT(); }
int64_t uwph_file_seek(struct uwph_object *o, int64_t off, int whence)
{ (void)o; (void)off; (void)whence; return UWPH_ENOSYS_INT(); }
int uwph_file_fstat(struct uwph_object *o, struct uwph_stat *st)
{ (void)o; (void)st; return UWPH_ENOSYS_INT(); }
#endif

UWPH_API int uwph_open(const char *path, int flags, ...)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    va_list ap; unsigned mode = 0;
    if (flags & (UWPH_O_CREAT | 020000000)) { va_start(ap, flags); mode = va_arg(ap, unsigned); va_end(ap); }
    return uwph_linux_open(path, flags, mode);
#else
    return host_open(path, flags);
#endif
}

UWPH_API int uwph_openat(int dirfd, const char *path, int flags, ...)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    va_list ap; unsigned mode = 0;
    if (flags & (UWPH_O_CREAT | 020000000)) { va_start(ap, flags); mode = va_arg(ap, unsigned); va_end(ap); }
    return uwph_linux_openat(dirfd, path, flags, mode);
#else
    if (dirfd != UWPH_AT_FDCWD && path[0] != '/') return UWPH_ENOSYS_INT();   /* needs dir fds */
    return host_open(path, flags);
#endif
}

UWPH_API int uwph_creat(const char *path, uwph_mode_t mode)
{
    return uwph_open(path, UWPH_O_CREAT | UWPH_O_WRONLY | UWPH_O_TRUNC, mode);
}

UWPH_API int uwph_fstat(int fd, struct uwph_stat *st)
{
    uwph_trace_hit(UWPH_TRACE_FSTAT);

#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_fstat(fd, st);
#else
    enum uwph_fd_kind kind;
    int ret;
    struct uwph_object *o = uwph_fd_get(fd, &kind, NULL);
    switch (kind) {
    case UWPH_FD_FREE: return UWPH_FAIL(UWPH_EBADF);
    case UWPH_FD_FILE: ret = uwph_file_fstat(o, st); break;
    case UWPH_FD_UNIX: fill_socket_stat(st); ret = 0; break;
    case UWPH_FD_PIPE_R: case UWPH_FD_PIPE_W:
        fill_socket_stat(st); st->st_mode = UWPH_S_IFIFO | 0600; ret = 0; break;
    case UWPH_FD_CONSOLE:
        fill_socket_stat(st); st->st_mode = UWPH_S_IFCHR | 0620; ret = 0; break;
    default: ret = UWPH_ENOSYS_INT();
    }
    uwph_obj_unref(o);
    return ret;
#endif
}

static int stat_path(const char *path, struct uwph_stat *st)
{
    int fd, ret;
    /* wineserver's socket: ntdll lstat()s it before connecting (server.c) */
    if (uwph_unix_path_listening(path)) { fill_socket_stat(st); return 0; }
#ifdef UWPH_BACKEND_WIN32
    /* Directories cannot be opened by host_open without O_DIRECTORY; answer
     * them from mapped path metadata. Files keep the handle-based fstat. */
    if (strncmp(path, "/proc/", 6)) {
        char host[1024]; WCHAR wide[1024]; WIN32_FILE_ATTRIBUTE_DATA data;
        LARGE_INTEGER time;
        int flags = uwph_fs_translate(path, host, sizeof(host));
        if (flags < 0) return UWPH_FAIL(-flags);
        if (!utf8_to_wide(host, wide, 1024)) return UWPH_FAIL(UWPH_ENAMETOOLONG);
        if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data))
            return UWPH_FAIL(win_errno(GetLastError()));
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            memset(st, 0, sizeof(*st));
            /* READONLY on a Windows directory is a shell hint, not a write ban */
            st->st_mode = UWPH_S_IFDIR | 0755;
            st->st_nlink = 1;
            st->st_uid = st->st_gid = uwph_getuid();
            st->st_blksize = 4096;
            time.HighPart = data.ftLastAccessTime.dwHighDateTime;
            time.LowPart = data.ftLastAccessTime.dwLowDateTime;
            st->st_atim = ft_to_ts(time);
            time.HighPart = data.ftLastWriteTime.dwHighDateTime;
            time.LowPart = data.ftLastWriteTime.dwLowDateTime;
            st->st_mtim = st->st_ctim = ft_to_ts(time);
            return 0;
        }
    }
#endif
    if ((fd = host_open(path, UWPH_O_RDONLY)) < 0) return -1;
    ret = uwph_fstat(fd, st);
    uwph_close(fd);
    return ret;
}

UWPH_API int uwph_stat(const char *path, struct uwph_stat *st)  {
    uwph_trace_hit(UWPH_TRACE_STAT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_stat(path, st);
#else
    return stat_path(path, st);
#endif
}
UWPH_API int uwph_lstat(const char *path, struct uwph_stat *st) {
    uwph_trace_hit(UWPH_TRACE_LSTAT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_lstat(path, st);
#else
    return stat_path(path, st);
#endif
}

UWPH_API int uwph_fstatat(int dirfd, const char *path, struct uwph_stat *st, int flags)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_fstatat(dirfd, path, st, flags);
#else
    if ((flags & UWPH_AT_EMPTY_PATH) && !path[0]) return uwph_fstat(dirfd, st);
    if (dirfd != UWPH_AT_FDCWD && path[0] != '/') return UWPH_ENOSYS_INT();
    return stat_path(path, st);
#endif
}

UWPH_API int uwph_access(const char *path, int mode)
{
    uwph_trace_hit(UWPH_TRACE_ACCESS);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "access", (const char *, int));
    UWPH_NATIVE_RETURN(fn(path, mode));
#endif
    struct uwph_stat st;
    if (mode & ~7) return UWPH_FAIL(UWPH_EINVAL);
    if (stat_path(path, &st) < 0) return -1;
    if (((mode & 4) && !(st.st_mode & 0444)) ||
        ((mode & 2) && !(st.st_mode & 0222)) ||
        ((mode & 1) && !(st.st_mode & 0111))) return UWPH_FAIL(UWPH_EACCES);
    if (mode & 2) {
        char host[1024]; int flags = uwph_fs_translate(path, host, sizeof(host));
        if (flags < 0) return UWPH_FAIL(-flags);
        if (flags & UWPH_MOUNT_RO) return UWPH_FAIL(UWPH_EACCES);
    }
    return 0;
}

/* ---- not implemented yet on any backend (ENOSYS) ---- */
#define FS_STUB(ret, name, args, ...) UWPH_API ret uwph_##name args { __VA_ARGS__; return UWPH_ENOSYS_INT(); }
#define FS_STUBP(ret, name, args, ...) UWPH_API ret uwph_##name args { __VA_ARGS__; return UWPH_ENOSYS_PTR(); }

FS_STUB(int, fstatfs, (int fd, struct uwph_statfs *st), (void)fd, (void)st)

FS_STUB(int, rmdir, (const char *p), (void)p)
FS_STUB(int, unlink, (const char *p), (void)p)
FS_STUB(int, remove, (const char *p), (void)p)
FS_STUB(int, rename, (const char *a, const char *b), (void)a, (void)b)
FS_STUB(int, link, (const char *a, const char *b), (void)a, (void)b)
FS_STUB(int, symlink, (const char *a, const char *b), (void)a, (void)b)
UWPH_API uwph_ssize_t uwph_readlink(const char *p, char *b, size_t l)
{
    if (!strcmp(p, "/proc/self/exe")) {
        char exe[1024];
        int n = uwph_fake_proc_exe(exe, sizeof(exe));
        if (n < 0) return -1;
        if ((size_t)n > l) n = (int)l;
        memcpy(b, exe, n); return n;
    }
    if (!strncmp(p, "/proc/", 6)) return uwph_fake_proc_link(p, b, l);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(uwph_ssize_t, "readlink", (const char *, char *, size_t));
    UWPH_NATIVE_RETURN(fn(p, b, l));
#endif
    return UWPH_ENOSYS_INT();
}

FS_STUB(int, fchmod, (int fd, uwph_mode_t m), (void)fd, (void)m)
FS_STUB(int, chmod, (const char *p, uwph_mode_t m), (void)p, (void)m)
FS_STUB(int, fsync, (int fd), (void)fd)
FS_STUB(int, ftruncate, (int fd, uwph_off_t l), (void)fd, (void)l)
FS_STUB(int, posix_fallocate, (int fd, uwph_off_t o, uwph_off_t l), (void)fd, (void)o, (void)l)
FS_STUB(int, futimens, (int fd, const struct uwph_timespec t[2]), (void)fd, (void)t)
FS_STUB(int, utimensat, (int d, const char *p, const struct uwph_timespec t[2], int f), (void)d, (void)p, (void)t, (void)f)
FS_STUBP(char *, mkdtemp, (char *t), (void)t)

FS_STUBP(uwph_DIR *, fdopendir, (int fd), (void)fd)


FS_STUB(uwph_ssize_t, getxattr, (const char *p, const char *n, void *v, size_t l), (void)p, (void)n, (void)v, (void)l)
FS_STUB(uwph_ssize_t, fgetxattr, (int fd, const char *n, void *v, size_t l), (void)fd, (void)n, (void)v, (void)l)
FS_STUB(int, fsetxattr, (int fd, const char *n, const void *v, size_t l, int f), (void)fd, (void)n, (void)v, (void)l, (void)f)
FS_STUB(int, fremovexattr, (int fd, const char *n), (void)fd, (void)n)
FS_STUB(int, inotify_init, (void))
FS_STUB(int, inotify_add_watch, (int fd, const char *p, uint32_t m), (void)fd, (void)p, (void)m)
FS_STUB(int, inotify_rm_watch, (int fd, int wd), (void)fd, (void)wd)

UWPH_API char *uwph_realpath(const char *path, char *resolved)
{
    uwph_trace_hit(UWPH_TRACE_REALPATH);
    if (!strcmp(path, "/proc/self/exe")) {
        char exe[1024];
        int n = uwph_fake_proc_exe(exe, sizeof(exe));
        if (n < 0) return NULL;
        if (!resolved && !(resolved = malloc((size_t)n + 1))) return UWPH_FAILP(UWPH_ENOMEM);
        memcpy(resolved, exe, (size_t)n + 1); return resolved;
    }
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(char *, "realpath", (const char *, char *));
    UWPH_NATIVE_RETURN(fn(path, resolved));
#endif

    char abs[1024];
    if (uwph_fs_abspath(path, abs, sizeof(abs)) < 0) return NULL;
    if (!resolved && !(resolved = malloc(strlen(abs) + 1))) return UWPH_FAILP(UWPH_ENOMEM);
    strcpy(resolved, abs);   /* no symlinks in the host tree yet */
    return resolved;
}

UWPH_API uwph_mode_t uwph_umask(uwph_mode_t mask)
{
    uwph_trace_hit(UWPH_TRACE_UMASK);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(uwph_mode_t, "umask", (uwph_mode_t));
    UWPH_NATIVE_RETURN(fn(mask));
#endif
    static uwph_mode_t cur = 022;
    uwph_mode_t old = cur;
    cur = mask & 0777;
    return old;
}

UWPH_API int uwph_mkdir(const char *path, uwph_mode_t mode)
{
    uwph_trace_hit(UWPH_TRACE_MKDIR);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "mkdir", (const char *, uwph_mode_t));
    UWPH_NATIVE_RETURN(fn(path, mode));
#elif defined(UWPH_BACKEND_WIN32)
    char host[1024]; WCHAR wide[1024];
    int flags = uwph_fs_translate(path, host, sizeof(host));
    if (flags < 0) return UWPH_FAIL(-flags);
    if (flags & UWPH_MOUNT_RO) return UWPH_FAIL(UWPH_EACCES);
    if (!utf8_to_wide(host, wide, 1024)) return UWPH_FAIL(UWPH_ENAMETOOLONG);
    return CreateDirectoryW(wide, NULL) ? 0 : UWPH_FAIL(win_errno(GetLastError()));
#else
    return UWPH_ENOSYS_INT();
#endif
}
UWPH_API int uwph_fchdir(int fd)
{
    uwph_trace_hit(UWPH_TRACE_FCHDIR);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_fchdir(fd);
#elif defined(UWPH_BACKEND_WIN32)
    enum uwph_fd_kind kind;
    struct uwph_object *o = uwph_fd_get(fd, &kind, NULL);
    if (!o) return UWPH_FAIL(UWPH_EBADF);
    struct hfile *file = (struct hfile *)o;
    int r = kind == UWPH_FD_FILE && file->directory ? uwph_chdir(file->directory) : UWPH_FAIL(UWPH_ENOTDIR);
    uwph_obj_unref(o);
    return r;
#else
    return UWPH_ENOSYS_INT();
#endif
}

#ifdef UWPH_BACKEND_WIN32
struct uwph_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    int first, ended;
    struct uwph_dirent entry;
};
#endif
UWPH_API uwph_DIR *uwph_opendir(const char *path)
{
    uwph_trace_hit(UWPH_TRACE_OPENDIR);
    if (path && (!strcmp(path, "/proc") || !strncmp(path, "/proc/", 6)))
        return uwph_fake_opendir(path);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(uwph_DIR *, "opendir", (const char *));
    UWPH_NATIVE_RETURN(fn(path));
#elif defined(UWPH_BACKEND_WIN32)
    char host[1024]; WCHAR wide[1024];
    int flags = uwph_fs_translate(path, host, sizeof(host));
    if (flags < 0) return UWPH_FAILP(-flags);
    if (!utf8_to_wide(host, wide, 1022)) return UWPH_FAILP(UWPH_ENAMETOOLONG);
    DWORD attrs = GetFileAttributesW(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES) return UWPH_FAILP(win_errno(GetLastError()));
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return UWPH_FAILP(UWPH_ENOTDIR);
    size_t n = wcslen(wide); wide[n++] = L'\\'; wide[n++] = L'*'; wide[n] = 0;
    uwph_DIR *dir = calloc(1, sizeof(*dir));
    if (!dir) return UWPH_FAILP(UWPH_ENOMEM);
    dir->find = FindFirstFileExW(wide, FindExInfoBasic, &dir->data, FindExSearchNameMatch, NULL, 0);
    if (dir->find == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) { free(dir); return UWPH_FAILP(win_errno(e)); }
        dir->ended = 1;
    }
    dir->first = 1;
    return dir;
#else
    return UWPH_ENOSYS_PTR();
#endif
}
UWPH_API struct uwph_dirent *uwph_readdir(uwph_DIR *dir)
{
    if (uwph_fake_dir_owned(dir)) return uwph_fake_readdir(dir);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(struct uwph_dirent *, "readdir", (uwph_DIR *));
    UWPH_NATIVE_RETURN(fn(dir));
#elif defined(UWPH_BACKEND_WIN32)
    if (dir->ended) return NULL;
    if (!dir->first && !FindNextFileW(dir->find, &dir->data)) {
        DWORD e = GetLastError(); dir->ended = 1;
        if (e != ERROR_NO_MORE_FILES) uwph_errno = win_errno(e);
        return NULL;
    }
    dir->first = 0;
    memset(&dir->entry, 0, sizeof(dir->entry));
    if (!WideCharToMultiByte(CP_UTF8, 0, dir->data.cFileName, -1, dir->entry.d_name,
                             sizeof(dir->entry.d_name), NULL, NULL)) return UWPH_FAILP(UWPH_ENAMETOOLONG);
    dir->entry.d_type = dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 4 : 8;
    dir->entry.d_reclen = sizeof(dir->entry);
    return &dir->entry;
#else
    return UWPH_ENOSYS_PTR();
#endif
}
UWPH_API int uwph_closedir(uwph_DIR *dir)
{
    if (uwph_fake_dir_owned(dir)) return uwph_fake_closedir(dir);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "closedir", (uwph_DIR *));
    UWPH_NATIVE_RETURN(fn(dir));
#elif defined(UWPH_BACKEND_WIN32)
    int r = dir->find == INVALID_HANDLE_VALUE || FindClose(dir->find) ? 0 : UWPH_FAIL(win_errno(GetLastError()));
    free(dir); return r;
#else
    return UWPH_ENOSYS_INT();
#endif
}
