/*
 * R4S O2 traced-set test: every symbol in the O1 priority list (the
 * uwph_trace_* set) is called through its public uwph entry point and must
 * both behave and bump its own counter. Same source for three builds:
 *   Linux stub        host-dependent entries fail with ENOSYS
 *   Linux passthrough harness archive, real host calls (no shim, no Wine)
 *   Windows           win32/UWP backend, run under wine64 here
 * argv[1] is the directory holding smoke-input.txt.
 */
#include "uwph/uwph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
# include <time.h>
# include <sys/socket.h>
# include <unistd.h>
#endif

static int fails, passes;

#define CHECK(cond, ...) do { \
    int ok_ = !!(cond); \
    if (ok_) passes++; else fails++; \
    printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
} while (0)

#if defined(UWPH_BACKEND_WIN32)
# define BACKEND "win32"
# define HOST 1
#elif defined(UWPH_BACKEND_LINUX_PASSTHROUGH)
# define BACKEND "linux-passthrough"
# define HOST 1
#else
# define BACKEND "linux-stub"
# define HOST 0
#endif

static uint64_t base[UWPH_TRACE_COUNT];
static char root[512], input[600];

static void test_format(void)
{
    char buf[16], *s = NULL;
    CHECK(uwph_snprintf(buf, sizeof(buf), "%s-%d-%llx", "ab", 42, 0x1fULL) == 8 && !strcmp(buf, "ab-42-1f"),
          "snprintf -> \"%s\"", buf);
    CHECK(uwph_snprintf(buf, 4, "%d", 123456) == 6 && !strcmp(buf, "123"), "snprintf truncates, returns full length");
    CHECK(uwph_asprintf(&s, "x=%d/%s", 7, "y") == 5 && s && !strcmp(s, "x=7/y"), "asprintf -> \"%s\"", s ? s : "(null)");
    free(s);
    CHECK(uwph_setlocale(UWPH_LC_ALL, NULL) != NULL, "setlocale(LC_ALL, NULL) query");
    s = uwph_setlocale(UWPH_LC_CTYPE, "C");
    CHECK(s && !strcmp(s, "C"), "setlocale(LC_CTYPE, \"C\") -> %s", s ? s : "(null)");
    s = uwph_setlocale(UWPH_LC_MESSAGES, "C");
    CHECK(s && !strcmp(s, "C"), "setlocale(LC_MESSAGES, \"C\") -> %s (Linux numbering)", s ? s : "(null)");
    uwph_errno = 0;
    CHECK(uwph_setlocale(99, "C") == NULL && uwph_errno == UWPH_EINVAL, "setlocale(bad category) -> EINVAL");
}

static void test_threads(void)
{
    uwph_pthread_key_t key;
    uwph_pthread_mutex_t m;
    uwph_pthread_mutexattr_t a;
    uwph_sigset_t set, old;
    int v = 5;

    CHECK(uwph_pthread_key_create(&key, NULL) == 0 && uwph_pthread_setspecific(key, &v) == 0 &&
          uwph_pthread_getspecific(key) == &v, "pthread_getspecific returns the set value");

    uwph_pthread_mutexattr_init(&a);
    uwph_pthread_mutexattr_settype(&a, UWPH_PTHREAD_MUTEX_RECURSIVE);
    uwph_pthread_mutex_init(&m, &a);
    CHECK(uwph_pthread_mutex_lock(&m) == 0 && uwph_pthread_mutex_lock(&m) == 0, "pthread_mutex_lock (recursive x2)");
    CHECK(uwph_pthread_mutex_unlock(&m) == 0 && uwph_pthread_mutex_unlock(&m) == 0, "pthread_mutex_unlock x2");

    uwph_sigemptyset(&set);
    CHECK(uwph_sigaddset(&set, UWPH_SIGUSR2) == 0 && uwph_sigismember(&set, UWPH_SIGUSR2) == 1,
          "sigaddset(SIGUSR2)");
    uwph_errno = 0;
    CHECK(uwph_sigaddset(&set, 0) == -1 && uwph_errno == UWPH_EINVAL, "sigaddset(0) -> EINVAL");
    CHECK(uwph_pthread_sigmask(UWPH_SIG_BLOCK, &set, NULL) == 0 &&
          uwph_pthread_sigmask(UWPH_SIG_BLOCK, NULL, &old) == 0 && uwph_sigismember(&old, UWPH_SIGUSR2) == 1,
          "pthread_sigmask BLOCK then query");
    CHECK(uwph_pthread_sigmask(UWPH_SIG_UNBLOCK, &set, &old) == 0 && uwph_sigismember(&old, UWPH_SIGUSR2) == 1 &&
          uwph_pthread_sigmask(UWPH_SIG_SETMASK, NULL, &old) == 0 && uwph_sigismember(&old, UWPH_SIGUSR2) == 0,
          "pthread_sigmask UNBLOCK restores");
}

static void test_process(void)
{
    char *e;
    uwph_time_t t = 0, r;
    CHECK(uwph_setenv("UWPH_O2_VAR", "traced", 1) == 0 && (e = uwph_getenv("UWPH_O2_VAR")) && !strcmp(e, "traced"),
          "getenv after setenv");
    uwph_unsetenv("UWPH_O2_VAR");
    CHECK(uwph_getenv("UWPH_O2_VAR") == NULL, "getenv(unset) -> NULL");
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    CHECK(uwph_getuid() == getuid(), "getuid -> host uid %u", (unsigned)uwph_getuid());
#else
    CHECK(uwph_getuid() == 1000, "getuid -> fixed app uid %u", (unsigned)uwph_getuid());
#endif
    r = uwph_time(&t);
#if HOST
    CHECK(r > 1600000000 && r == t, "time -> %lld (stored too)", (long long)r);
# ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    CHECK(r - time(NULL) <= 1 && time(NULL) - r <= 1, "time matches host clock");
# endif
#else
    CHECK(r == -1 && uwph_errno == UWPH_ENOSYS, "stub: time -> ENOSYS");
#endif
}

/* Byte channel for writev/fcntl/FILE scans: a uwph pipe, or on the
 * passthrough backend (no synthetic pipe in the native FILE world) a file. */
static void test_io(void)
{
    static const char text[] = "42 -7 1f 0x10 ztailend\n";
    struct uwph_iovec iov[2] = { { (void *)text, 9 }, { (void *)(text + 9), sizeof(text) - 10 } };
    unsigned u = 0; int d = 0, i = 0, n = 0; unsigned x = 0; char c = 0, w[8] = "", rest[8] = "";
    int rfd, wfd, dup;
    uwph_FILE *f;
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    char path[128];
    uwph_snprintf(path, sizeof(path), "/tmp/uwph-r4s-o2-%ld", (long)getpid());
    wfd = uwph_open(path, UWPH_O_CREAT | UWPH_O_EXCL | UWPH_O_RDWR, 0600);
    rfd = -1;
    CHECK(wfd >= 0, "fixture file open -> %d", wfd);
#else
    int p[2];
    CHECK(uwph_pipe(p) == 0, "pipe");
    rfd = p[0]; wfd = p[1];
#endif
    CHECK(uwph_writev(wfd, iov, 2) == (uwph_ssize_t)sizeof(text) - 1, "writev 2 iovecs");
    CHECK(uwph_write(wfd, "", 0) == 0, "write(0 bytes)");
    CHECK(uwph_write(-5, "x", 1) == -1 && uwph_errno == UWPH_EBADF, "write(bad fd) -> EBADF");
    CHECK((uwph_fcntl(wfd, UWPH_F_GETFL) & UWPH_O_ACCMODE) == (HOST && rfd < 0 ? UWPH_O_RDWR : UWPH_O_WRONLY),
          "fcntl F_GETFL (no argument read)");
    CHECK(uwph_fcntl(wfd, UWPH_F_SETFD, UWPH_FD_CLOEXEC) == 0 && uwph_fcntl(wfd, UWPH_F_GETFD) == UWPH_FD_CLOEXEC,
          "fcntl F_SETFD/F_GETFD");
    dup = uwph_fcntl(wfd, UWPH_F_DUPFD_CLOEXEC, 50);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    CHECK(dup == -1 && uwph_errno == UWPH_EINVAL, "fcntl F_DUPFD_CLOEXEC refused (private slots)");
    CHECK(uwph_lseek(wfd, 0, UWPH_SEEK_SET) == 0, "rewind uwph descriptor");
    CHECK(uwph_fdopen(wfd, "invalid") == NULL, "failed fdopen rejects invalid mode");
    CHECK(uwph_fcntl(wfd, UWPH_F_GETFD) >= 0, "failed fdopen retains descriptor ownership");
    f = uwph_fdopen(wfd, "r");
    CHECK(f && uwph_fileno(f) == wfd, "fdopen/fileno preserves private descriptor");
#else
    CHECK(dup >= 50 && uwph_fcntl(dup, UWPH_F_GETFD) == UWPH_FD_CLOEXEC, "fcntl F_DUPFD_CLOEXEC -> %d", dup);
    if (dup >= 0) uwph_close(dup);
    uwph_close(wfd);
    f = uwph_fdopen(rfd, "r");
#endif
    CHECK(f != NULL, "FILE over written bytes");
    if (!f) return;
    CHECK(uwph_fscanf(f, "%u %d %x %i %c%4s%n", &u, &d, &x, &i, &c, w, &n) == 6 &&
          u == 42 && d == -7 && x == 0x1f && i == 16 && c == 'z' && !strcmp(w, "tail") && n == 19,
          "fscanf %%u %%d %%x %%i %%c %%4s %%n -> %u %d %#x %d %c %s n=%d", u, d, x, i, c, w, n);
    CHECK(!uwph_feof(f), "feof before end -> 0");
    CHECK(uwph_fscanf(f, "%7s", rest) == 1 && !strcmp(rest, "end"), "fscanf rest -> %s", rest);
    CHECK(uwph_fscanf(f, "%7s", rest) == -1 && uwph_feof(f), "fscanf at end -> EOF, feof -> 1");
    CHECK(uwph_fclose(f) == 0, "fclose");
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    CHECK(uwph_close(wfd) == -1 && uwph_errno == UWPH_EBADF, "fclose consumes fdopen descriptor");
    f = uwph_fopen(path, "r");
    CHECK(f != NULL, "fopen composition fixture");
    if (f) {
        int fd = uwph_fileno(f);
        char bytes[3] = {0};
        CHECK(fd >= 0 && fd == uwph_fileno(f) && uwph_read(fd, bytes, 2) == 2 && !strcmp(bytes, "42"),
              "fopen/fileno/read shares descriptor namespace");
        CHECK(uwph_fclose(f) == 0 && uwph_read(fd, bytes, 1) == -1 && uwph_errno == UWPH_EBADF,
              "fclose invalidates fileno descriptor");
        int next = uwph_open(path, UWPH_O_RDONLY, 0);
        CHECK(next >= 0 && uwph_read(next, bytes, 2) == 2, "descriptor usable after slot reuse");
        if (next >= 0) uwph_close(next);
    }
    unlink(path); /* fixture cleanup only */
#endif
}

/* Linux x64 long remains eight bytes even on the Win32 LLP64 host. */
static void test_scan_long(void)
{
#ifndef UWPH_BACKEND_LINUX_PASSTHROUGH
    int p[2];
    const char text[] = "123456789abcdef0 -4294967297 4294967298";
    CHECK(uwph_pipe(p) == 0, "long scanner pipe");
    uwph_write(p[1], text, sizeof(text) - 1);
    uwph_close(p[1]);
    uwph_FILE *f = uwph_fdopen(p[0], "r");
    CHECK(f != NULL, "long scanner fdopen");
    if (f) {
        uint64_t x = UINT64_C(0xaaaaaaaaaaaaaaaa), u = x;
        int64_t d = (int64_t)x, n = (int64_t)x;
        CHECK(uwph_fscanf(f, "%lx %ld %lu%ln", &x, &d, &u, &n) == 3 &&
              x == UINT64_C(0x123456789abcdef0) && d == -INT64_C(4294967297) &&
              u == UINT64_C(4294967298) && n == (int64_t)sizeof(text) - 1,
              "Linux ABI %%lx/%%ld/%%lu/%%ln replaces all eight bytes");
        uwph_fclose(f);
    }
#endif
}

static void test_files(void)
{
    struct uwph_stat st;
    char a[16] = "", b[16] = "", tail[16] = "", *r;
    uwph_FILE *f = uwph_fopen(input, "r");
#if HOST
    CHECK(f != NULL, "fopen(%s)", input);
    if (f) {
        CHECK(uwph_fscanf(f, "%15s %15s input: %*s %*s %15s", a, b, tail) == 3 &&
              !strcmp(a, "uwph") && !strcmp(b, "smoke") && !strcmp(tail, "CreateFile2"),
              "fscanf fixture -> %s %s ... %s", a, b, tail);
        CHECK(uwph_fscanf(f, "%15s", a) == -1 && uwph_feof(f), "fixture EOF");
        CHECK(uwph_fclose(f) == 0, "fclose fixture");
    }
    CHECK(uwph_stat(input, &st) == 0 && (st.st_mode & UWPH_S_IFMT) == UWPH_S_IFREG && st.st_size == 43,
          "stat fixture: regular, %lld bytes", (long long)st.st_size);
#else
    CHECK(f == NULL && uwph_errno == UWPH_ENOSYS, "stub: fopen -> ENOSYS");
    CHECK(uwph_stat(input, &st) == -1 && uwph_errno == UWPH_ENOSYS, "stub: stat -> ENOSYS");
    (void)a; (void)b; (void)tail;
#endif
    uwph_errno = 0;
    CHECK(uwph_fopen("/uwph-o2-missing", "r") == NULL && uwph_errno == (HOST ? UWPH_ENOENT : UWPH_ENOSYS),
          "fopen(missing) -> errno %d", uwph_errno);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    {
        char dotted[640], *canon = uwph_realpath(root, NULL);
        uwph_snprintf(dotted, sizeof(dotted), "%s/./smoke-input.txt", root);
        r = uwph_realpath(dotted, NULL);
        CHECK(canon && r && !strncmp(r, canon, strlen(canon)) && !strcmp(r + strlen(canon), "/smoke-input.txt"),
              "realpath -> %s", r ? r : "(null)");
        free(r); free(canon);
        CHECK(uwph_realpath("/uwph-o2-missing", NULL) == NULL && uwph_errno == UWPH_ENOENT, "realpath(missing) -> ENOENT");
    }
#else
    r = uwph_realpath("/app/lib/../lib/./wine", NULL);
    CHECK(r && !strcmp(r, "/app/lib/wine"), "realpath -> %s", r ? r : "(null)");
    free(r);
#endif
}

static void test_b06(void)
{
    struct uwph_rlimit lim;
    struct uwph_stat st;
    uwph_Dl_info info;
    uwph_sigset_t set;
    int p[2], value=1024;
    char *argv[] = { "missing", NULL };
    static char env[] = "UWPH_B06=value";
    CHECK(uwph_putenv(env)==0 && !strcmp(uwph_getenv("UWPH_B06"),"value"), "B06 putenv shares getenv table");
    uwph_unsetenv("UWPH_B06");
    CHECK(uwph_getrlimit(UWPH_RLIMIT_NOFILE,&lim)==0 && lim.rlim_cur>0, "B06 getrlimit NOFILE");
    CHECK(uwph_setrlimit(UWPH_RLIMIT_NOFILE,&lim)==0, "B06 setrlimit unchanged NOFILE");
#ifndef UWPH_BACKEND_LINUX_PASSTHROUGH
    CHECK(uwph_setrlimit(UWPH_RLIMIT_STACK,&lim)==-1 && uwph_errno==UWPH_ENOSYS,
          "B06 setrlimit unsupported host limit -> ENOSYS");
    struct uwph_rlimit invalid={2,1};
    CHECK(uwph_setrlimit(UWPH_RLIMIT_NOFILE,&invalid)==-1 && uwph_errno==UWPH_EINVAL,
          "B06 setrlimit rejects soft greater than hard");
#endif
    CHECK(uwph_sysconf(UWPH_SC_PAGESIZE)==4096, "B06 sysconf page size");
    CHECK(uwph_sigemptyset(&set)==0 && uwph_sigismember(&set,UWPH_SIGUSR2)==0, "B06 sigemptyset clears membership");
    CHECK(uwph_execv("/uwph-b06-missing",argv)==-1 && uwph_errno==
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
          UWPH_ENOENT,
#else
          UWPH_ENOSYS,
#endif
          "B06 execv missing / UWP ENOSYS");
    CHECK(uwph_dladdr(NULL,&info)==0, "B06 dladdr unknown address");
    CHECK(uwph_chdir("/")==0, "B06 chdir root");
    CHECK(uwph_fstat(-1,&st)==-1 && uwph_errno==UWPH_EBADF, "B06 fstat invalid descriptor");
    CHECK(uwph_isatty(-1)==0 && uwph_errno==UWPH_EBADF, "B06 isatty invalid descriptor");
    CHECK(uwph_pipe2(p,UWPH_O_CLOEXEC)==0, "B06 pipe2 CLOEXEC");
    CHECK(uwph_fcntl(p[0],UWPH_F_GETFD)==UWPH_FD_CLOEXEC, "B06 pipe2 preserves CLOEXEC");
    CHECK(uwph_write(p[1],"b",1)==1, "B06 pipe write");
    char b=0;
    CHECK(uwph_read(p[0],&b,1)==1 && b=='b', "B06 pipe read");
    uwph_close(p[0]); uwph_close(p[1]);
    struct uwph_msghdr msg; memset(&msg,0,sizeof(msg));
    CHECK(uwph_sendmsg(-1,&msg,0)==-1 && uwph_errno==UWPH_EBADF, "B06 sendmsg invalid descriptor");
    CHECK(uwph_recvmsg(-1,&msg,0)==-1 && uwph_errno==UWPH_EBADF, "B06 recvmsg invalid descriptor");
    CHECK(uwph_setsockopt(-1,1,8,&value,sizeof(value))==-1 && uwph_errno==UWPH_EBADF, "B06 setsockopt invalid descriptor");
    int sv[2];
    int pair_ok=uwph_socketpair(UWPH_AF_UNIX,UWPH_SOCK_STREAM,0,sv);
    CHECK(pair_ok==0, "B06 socket pair fixture");
    if (pair_ok==0) {
        struct uwph_iovec v={ (void *)"ok", 2 };
        msg.msg_iov=&v; msg.msg_iovlen=1;
        CHECK(uwph_setsockopt(sv[0],UWPH_SOL_SOCKET,8,&value,sizeof(value))==0, "B06 setsockopt receive buffer");
        CHECK(uwph_sendmsg(sv[0],&msg,0)==2, "B06 sendmsg payload");
        char received[3]={0}; v.iov_base=received;
        CHECK(uwph_recvmsg(sv[1],&msg,0)==2 && !strcmp(received,"ok"), "B06 recvmsg payload");
        CHECK(uwph_close(sv[0])==0 && uwph_close(sv[1])==0, "B06 socket pair uwph close");
    }
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    /* Exercise the other descriptor producers, not just a native fixture. */
    struct uwph_sockaddr_un address={UWPH_AF_UNIX,{0}};
    uwph_snprintf(address.sun_path,sizeof(address.sun_path),"/tmp/uwph-b06-%d.sock",uwph_getpid());
    int listener=uwph_socket(UWPH_AF_UNIX,UWPH_SOCK_STREAM,0);
    int client=uwph_socket(UWPH_AF_UNIX,UWPH_SOCK_STREAM,0);
    CHECK(listener>=0 && client>=0, "B06 uwph socket producers");
    int bound=uwph_bind(listener,(struct uwph_sockaddr *)&address,sizeof(address));
    CHECK(bound==0, "B06 uwph bind native lifecycle");
    if (bound==0) {
        CHECK(uwph_listen(listener,1)==0, "B06 uwph listen native lifecycle");
        int connected=uwph_connect(client,(struct uwph_sockaddr *)&address,sizeof(address));
        CHECK(connected==0, "B06 uwph connect native lifecycle");
        if (connected==0) {
            int peer=uwph_accept(listener,NULL,NULL);
            CHECK(peer>=0, "B06 uwph accept producer");
            int kind=0; uwph_socklen_t length=sizeof(kind);
            CHECK(uwph_getsockopt(peer,UWPH_SOL_SOCKET,UWPH_SO_TYPE,&kind,&length)==0 && kind==UWPH_SOCK_STREAM,
                  "B06 uwph getsockopt accepted descriptor");
            struct uwph_sockaddr_un name; length=sizeof(name);
            CHECK(uwph_getsockname(peer,(struct uwph_sockaddr *)&name,&length)==0, "B06 uwph getsockname");
            length=sizeof(name);
            CHECK(uwph_getpeername(peer,(struct uwph_sockaddr *)&name,&length)==0, "B06 uwph getpeername");
            CHECK(uwph_send(client,"x",1,0)==1, "B06 uwph send created descriptor");
            char payload=0;
            CHECK(uwph_recv(peer,&payload,1,0)==1 && payload=='x', "B06 uwph recv accepted descriptor");
            CHECK(uwph_shutdown(client,UWPH_SHUT_WR)==0 && uwph_recv(peer,&payload,1,0)==0,
                  "B06 uwph shutdown delivers EOF");
            CHECK(uwph_close(peer)==0, "B06 uwph close accepted descriptor");
        }
        uwph_unlink(address.sun_path);
    }
    uwph_close(client); uwph_close(listener);
#endif
    uwph_FILE *f=uwph_fopen(input,"r");
    if (f) { uwph_setbuf(f,NULL); CHECK(uwph_fread(&b,1,1,f)==1 && b=='u', "B06 setbuf stream remains readable"); uwph_fclose(f); }
#if !HOST
    else { int q[2]; uwph_pipe(q); f=uwph_fdopen(q[0],"r"); uwph_setbuf(f,NULL); uwph_close(q[1]); uwph_fclose(f); }
#endif
}

int main(int argc, char **argv)
{
    uwph_snprintf(root, sizeof(root), "%s", argc > 1 ? argv[1] : "x64/uwp-host/tests");
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    uwph_snprintf(input, sizeof(input), "%s/smoke-input.txt", root);
#else
    CHECK(uwph_init(root, HOST ? "C:\\uwph-install" : "/opt/uwph-install") == 0, "uwph_init root=%s", root);
    uwph_snprintf(input, sizeof(input), "/smoke-input.txt");
#endif
    printf("uwph traced-set test, backend: %s (built as %s)\n", uwph_backend_name(), BACKEND);
    for (int s = 0; s < UWPH_TRACE_COUNT; s++) base[s] = uwph_trace_count(s);

    test_b06();
    test_format();
    test_threads();
    test_process();
    test_io();
    test_files();
    test_scan_long();

    CHECK(uwph_trace_name(UWPH_TRACE_COUNT) == NULL && uwph_trace_count(UWPH_TRACE_COUNT) == 0,
          "trace API bounds");
    for (int s = 0; s < UWPH_TRACE_COUNT; s++) {
        uint64_t delta = uwph_trace_count(s) - base[s];
        CHECK(uwph_trace_name(s) && delta > 0, "counter %-20s +%llu", uwph_trace_name(s), (unsigned long long)delta);
    }
    printf("%d passed, %d failed (%d traced symbols)\n", passes, fails, (int)UWPH_TRACE_COUNT);
    return fails ? 1 : 0;
}
