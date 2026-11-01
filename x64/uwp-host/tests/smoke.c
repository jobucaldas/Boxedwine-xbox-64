/*
 * uwph smoke test. Same source for both builds:
 *   Linux (stub backend): portable parts must work, host calls must say ENOSYS.
 *   Windows (win32 backend, run under wine64 here, on the Xbox later): the
 *   implemented host calls must work for real.
 */
#include "internal.h"   /* white-box: host threads, CONTEXT layout */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

long unixside_roundtrip(const char *msg);
long unixside_syscall_gettid(void);

static int fails, passes;

#define CHECK(cond, ...) do { \
    int ok_ = !!(cond); \
    if (ok_) passes++; else fails++; \
    printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
} while (0)

#ifdef UWPH_BACKEND_WIN32
# define WIN 1
#else
# define WIN 0
#endif

/* ---- fd table, pipes ---- */
static void test_fds(void)
{
    int p[2], d, d2;
    char buf[16];
    struct uwph_pollfd pf;

    CHECK(uwph_pipe(p) == 0, "pipe -> %d,%d", p[0], p[1]);
    d = uwph_dup(p[1]);
    d2 = uwph_dup2(p[0], 100);
    CHECK(d > p[1] && d2 == 100, "dup -> %d, dup2 -> %d", d, d2);
    CHECK(uwph_fcntl(d, UWPH_F_SETFD, UWPH_FD_CLOEXEC) == 0 && uwph_fcntl(d, UWPH_F_GETFD) == UWPH_FD_CLOEXEC,
          "F_SETFD/F_GETFD cloexec");
    pf.fd = 100; pf.events = UWPH_POLLIN;
    CHECK(uwph_poll(&pf, 1, 0) == 0, "poll empty pipe: not readable");
    CHECK(uwph_write(d, "hello", 5) == 5, "write via dup'd write end");
    CHECK(uwph_poll(&pf, 1, 0) == 1 && (pf.revents & UWPH_POLLIN), "poll: readable (revents %#x)", pf.revents);
    CHECK(uwph_read(100, buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5), "read via dup2'd read end");
    uwph_close(p[1]);
    uwph_close(d);
    CHECK(uwph_read(p[0], buf, sizeof(buf)) == 0, "EOF after all write ends closed");
    CHECK(uwph_write(1000, "x", 1) == -1 && uwph_errno == UWPH_EBADF, "write(bad fd) -> EBADF");
    uwph_fcntl(p[0], UWPH_F_SETFL, UWPH_O_NONBLOCK);
    uwph_close(p[0]);
    uwph_close(100);
}

/* ---- socketpair + SCM_RIGHTS (wine_server_send_fd / receive_fd) ---- */
static int send_fd(int sock, int fd, const void *data, size_t len)
{
    char cbuf[UWPH_CMSG_SPACE(sizeof(int))];
    struct uwph_iovec iov = { (void *)data, len };
    struct uwph_msghdr m = { NULL, 0, &iov, 1, cbuf, sizeof(cbuf), 0 };
    struct uwph_cmsghdr *c = (struct uwph_cmsghdr *)cbuf;
    c->cmsg_len = UWPH_CMSG_LEN(sizeof(int));
    c->cmsg_level = UWPH_SOL_SOCKET;
    c->cmsg_type = UWPH_SCM_RIGHTS;
    memcpy(UWPH_CMSG_DATA(c), &fd, sizeof(int));
    return (int)uwph_sendmsg(sock, &m, 0);
}

static int recv_fd(int sock, void *data, size_t len, int *fd)
{
    char cbuf[UWPH_CMSG_SPACE(sizeof(int))];
    struct uwph_iovec iov = { data, len };
    struct uwph_msghdr m = { NULL, 0, &iov, 1, cbuf, sizeof(cbuf), 0 };
    int n = (int)uwph_recvmsg(sock, &m, 0);
    struct uwph_cmsghdr *c = UWPH_CMSG_FIRSTHDR(&m);
    *fd = -1;
    if (n >= 0 && c && c->cmsg_type == UWPH_SCM_RIGHTS) memcpy(fd, UWPH_CMSG_DATA(c), sizeof(int));
    return n;
}

static void test_scm_rights(void)
{
    int sv[2], p[2], got;
    unsigned int tid = 0x1234;
    char buf[8];

    CHECK(uwph_socketpair(UWPH_AF_UNIX, UWPH_SOCK_STREAM, 0, sv) == 0, "socketpair");
    uwph_pipe(p);
    CHECK(send_fd(sv[0], p[1], &tid, sizeof(tid)) == sizeof(tid), "sendmsg with SCM_RIGHTS(pipe write end)");
    uwph_close(p[1]);   /* only the in-flight copy keeps it alive now */
    tid = 0;
    CHECK(recv_fd(sv[1], &tid, sizeof(tid), &got) == sizeof(tid) && tid == 0x1234 && got >= 0,
          "recvmsg: data %#x, received fd %d", tid, got);
    CHECK(uwph_write(got, "via-fd", 6) == 6 && uwph_read(p[0], buf, sizeof(buf)) == 6 && !memcmp(buf, "via-fd", 6),
          "write through the passed fd arrives at the original pipe");
    uwph_close(got);
    CHECK(uwph_read(p[0], buf, sizeof(buf)) == 0, "pipe EOF once the passed fd is closed");
    uwph_close(sv[0]);
    CHECK(uwph_read(sv[1], buf, sizeof(buf)) == 0, "socketpair EOF after peer close");
    uwph_close(sv[1]);
    uwph_close(p[0]);
}

/* ---- named AF_UNIX: the wineserver handshake shape ---- */
static const char *sock_dir = "/tmp/.wine-1000/server-fd-1";

static void *server_main_thread(void *arg)
{
    int l = *(int *)arg, c, ep, n, p[2];
    struct uwph_epoll_event ev = { UWPH_POLLIN, 42 }, out;
    unsigned int version = 851;   /* SERVER_PROTOCOL_VERSION of wine-10.0 */

    ep = uwph_epoll_create1(UWPH_EPOLL_CLOEXEC);
    uwph_epoll_ctl(ep, UWPH_EPOLL_CTL_ADD, l, &ev);
    n = uwph_epoll_wait(ep, &out, 1, 5000);
    if (n != 1 || out.data != 42) return (void *)1;
    if ((c = uwph_accept(l, NULL, NULL)) < 0) return (void *)2;
    uwph_pipe(p);
    /* the server sends its version and a pipe to the client */
    if (send_fd(c, p[0], &version, sizeof(version)) != sizeof(version)) return (void *)3;
    uwph_close(p[0]);
    if (uwph_write(p[1], "reply", 5) != 5) return (void *)4;
    uwph_close(p[1]);
    uwph_close(c);
    uwph_close(ep);
    return NULL;
}

static void test_named_socket(void)
{
    struct uwph_sockaddr_un addr = { UWPH_AF_UNIX, "socket" };
    struct uwph_stat st;
    int l, c, rfd = -1, arg;
    unsigned int version = 0;
    char buf[8];
    uwph_host_thread *thread;

    uwph_chdir(sock_dir);   /* ntdll does fchdir(server_dir) then connects to "socket" */
    l = uwph_socket(UWPH_AF_UNIX, UWPH_SOCK_STREAM | UWPH_SOCK_CLOEXEC, 0);
    CHECK(l >= 0 && uwph_bind(l, (struct uwph_sockaddr *)&addr, sizeof(addr)) == 0 && uwph_listen(l, 16) == 0,
          "server: socket/bind/listen %s/socket", sock_dir);
    CHECK(uwph_lstat("socket", &st) == 0 && (st.st_mode & UWPH_S_IFMT) == UWPH_S_IFSOCK,
          "lstat(\"socket\") reports S_IFSOCK (mode %o)", st.st_mode);
    CHECK(uwph_unix_path_listening("/tmp/.wine-1000/server-fd-1/socket"), "listener visible by absolute path");
    arg = l;
    thread = uwph_host_thread_start(server_main_thread, &arg);
    c = uwph_socket(UWPH_AF_UNIX, UWPH_SOCK_STREAM, 0);
    CHECK(uwph_connect(c, (struct uwph_sockaddr *)&addr, sizeof(addr)) == 0, "client: connect(\"socket\")");
    CHECK(recv_fd(c, &version, sizeof(version), &rfd) == sizeof(version) && version == 851 && rfd >= 0,
          "client: got protocol version %u and fd %d", version, rfd);
    CHECK(rfd >= 0 && uwph_read(rfd, buf, sizeof(buf)) == 5 && !memcmp(buf, "reply", 5), "client: read reply pipe");
    CHECK(uwph_host_thread_join(thread) == NULL, "server thread: epoll_wait/accept/sendmsg ok");
    uwph_close(rfd);
    uwph_close(c);
    uwph_close(l);
    CHECK(!uwph_unix_path_listening("/tmp/.wine-1000/server-fd-1/socket"), "listener gone after close");
    addr.sun_path[0] = 'x';
    c = uwph_socket(UWPH_AF_UNIX, UWPH_SOCK_STREAM, 0);
    CHECK(uwph_connect(c, (struct uwph_sockaddr *)&addr, sizeof(addr)) == -1 && uwph_errno == UWPH_ECONNREFUSED,
          "connect to missing socket -> ECONNREFUSED");
    uwph_close(c);
    uwph_chdir("/");
}

/* ---- dl registry ---- */
static int fake_unix_call(void *args) { return *(int *)args + 1; }

static void test_dl(void)
{
    static const struct uwph_dl_symbol syms[] = {
        { "__wine_unix_call_funcs", (void *)fake_unix_call }, { NULL, NULL } };
    static const struct uwph_dl_library lib = { "winevulkan.so", syms, (void *)fake_unix_call, (char *)fake_unix_call + 64 };
    int (*fn)(void *);
    int v = 41;
    void *h;
    uwph_Dl_info info;

    uwph_dl_register(&lib);
    h = uwph_dlopen("/app/lib/wine/x86_64-unix/winevulkan.so", 0);
    fn = (int (*)(void *))uwph_dlsym(h, "__wine_unix_call_funcs");
    CHECK(h && fn && fn(&v) == 42, "dlopen/dlsym from the static registry");
    CHECK(!uwph_dlopen("libGL.so.1", 0) && uwph_dlerror() != NULL, "dlopen of an unregistered library fails");
    CHECK(uwph_dladdr((char *)fake_unix_call + 4, &info) && !strcmp(info.dli_fname, "winevulkan.so"),
          "dladdr -> %s", info.dli_fname);
}

/* ---- exception -> signal mapping, context conversion ---- */
static int seen_sig;
static uwph_siginfo_t seen_si;
static int64_t seen_err, seen_trap, seen_rip;

static void on_signal(int sig, uwph_siginfo_t *si, void *ucv)
{
    uwph_ucontext_t *uc = ucv;
    seen_sig = sig;
    seen_si = *si;
    seen_err = uc->uc_mcontext.gregs[UWPH_REG_ERR];
    seen_trap = uc->uc_mcontext.gregs[UWPH_REG_TRAPNO];
    seen_rip = uc->uc_mcontext.gregs[UWPH_REG_RIP];
    uc->uc_mcontext.gregs[UWPH_REG_RAX] = 0x1111;     /* handler edits the context */
    uc->uc_mcontext.gregs[UWPH_REG_RIP] += 2;
}

static void test_signals(void)
{
    static const struct { uint32_t code; int sig; int trap; } map[] = {
        { UWPH_EXC_ACCESS_VIOLATION, UWPH_SIGSEGV, UWPH_TRAP_PAGEFLT },
        { UWPH_EXC_PRIV_INSTRUCTION, UWPH_SIGSEGV, UWPH_TRAP_PROTFLT },
        { UWPH_EXC_ILLEGAL_INSTRUCTION, UWPH_SIGILL, UWPH_TRAP_PRIVINFLT },
        { UWPH_EXC_INT_DIVIDE_BY_ZERO, UWPH_SIGFPE, UWPH_TRAP_DIVIDE },
        { UWPH_EXC_FLT_MULTIPLE_TRAPS, UWPH_SIGFPE, UWPH_TRAP_XMMFLT },
        { UWPH_EXC_BREAKPOINT, UWPH_SIGTRAP, UWPH_TRAP_BPTFLT },
        { UWPH_EXC_SINGLE_STEP, UWPH_SIGTRAP, UWPH_TRAP_DEBUG },
        { UWPH_EXC_DATATYPE_MISALIGNMENT, UWPH_SIGBUS, UWPH_TRAP_ALIGNFLT },
    };
    struct uwph_sigaction act;
    struct uwph_exception e;
    uwph_siginfo_t si;
    int64_t trap, err;
    int ok = 1;
    static uwph_win_context ctxbuf;
    uint64_t *rip = &ctxbuf.Rip, *rax = &ctxbuf.Rax;

    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        memset(&e, 0, sizeof(e));
        e.code = map[i].code;
        if (uwph_exception_to_siginfo(&e, &si, &trap, &err) != map[i].sig || trap != map[i].trap) ok = 0;
    }
    CHECK(ok, "exception code -> signal/trapno table (%u codes)", (unsigned)(sizeof(map) / sizeof(map[0])));
    memset(&e, 0, sizeof(e));
    e.code = 0xE06D7363;   /* C++ throw */
    CHECK(uwph_exception_to_siginfo(&e, &si, &trap, &err) == 0, "C++ exception is not turned into a signal");

    memset(&act, 0, sizeof(act));
    act.u.sigaction = on_signal;
    act.sa_flags = UWPH_SA_SIGINFO | UWPH_SA_ONSTACK;
    uwph_sigaction(UWPH_SIGSEGV, &act, NULL);
    uwph_sigaction(UWPH_SIGTRAP, &act, NULL);

    /* write fault at 0xdead0000 from rip 0x401000 */
    memset(&ctxbuf, 0, sizeof(ctxbuf));
    *rip = 0x401000;
    memset(&e, 0, sizeof(e));
    e.code = UWPH_EXC_ACCESS_VIOLATION;
    e.address = 0x401000;
    e.nparams = 2;
    e.info[0] = 1;
    e.info[1] = 0xdead0000;
    CHECK(uwph_dispatch_exception(&e, &ctxbuf) == 1 && seen_sig == UWPH_SIGSEGV &&
          seen_si.u.addr == (void *)0xdead0000 && ((seen_err >> 1) & 9) == 1 && seen_trap == UWPH_TRAP_PAGEFLT,
          "AV(write) -> SIGSEGV addr %p, Wine decodes info[0]=%lld", seen_si.u.addr, (long long)((seen_err >> 1) & 9));
    CHECK(*rax == 0x1111 && *rip == 0x401002, "handler's ucontext edits land in CONTEXT (rax %#llx rip %#llx)",
          (unsigned long long)*rax, (unsigned long long)*rip);
    e.info[0] = 8;
    uwph_dispatch_exception(&e, &ctxbuf);
    CHECK(((seen_err >> 1) & 9) == 8, "AV(execute) -> Wine decodes EXCEPTION_EXECUTE_FAULT");

    *rip = 0x402000;
    memset(&e, 0, sizeof(e));
    e.code = UWPH_EXC_BREAKPOINT;
    e.address = 0x402000;
    uwph_dispatch_exception(&e, &ctxbuf);
    CHECK(seen_sig == UWPH_SIGTRAP && seen_rip == 0x402001, "int3: Linux-style rip after the instruction (%#llx)",
          (unsigned long long)seen_rip);
}

/* ---- W^X policy ---- */
static void test_wx_policy(void)
{
    static char region[3 * 4096] __attribute__((aligned(4096)));
    int st = 0;
    void *page = NULL;
    uwph_wx_track(region, sizeof(region));
    CHECK(uwph_wx_decide(region + 4096 + 10, UWPH_FAULT_EXEC, &st, &page) && st == UWPH_WX_RX &&
          page == region + 4096, "W^X: exec fault on RW page -> flip page to RX");
    CHECK(!uwph_wx_decide(region + 4096, UWPH_FAULT_EXEC, &st, &page), "W^X: exec fault on RX page -> genuine fault");
    CHECK(uwph_wx_decide(region + 4096, UWPH_FAULT_WRITE, &st, &page) && st == UWPH_WX_RW, "W^X: write fault on RX page -> RW");
    CHECK(!uwph_wx_decide(region + 3 * 4096, UWPH_FAULT_EXEC, &st, &page), "W^X: untracked address -> not ours");
    uwph_wx_untrack(region, sizeof(region));
}

/* ---- TEB swap set on a fake TEB ---- */
static void test_teb_swap(void)
{
    uint64_t host_teb[0x100], wine_teb[0x100];
    struct uwph_teb_swap s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < 0x100; i++) { host_teb[i] = 0x1000 + i; wine_teb[i] = 0x2000 + i; }
    uwph_teb_capture_guest(wine_teb, &s);
    uwph_teb_enter_guest(host_teb, &s);
    CHECK(host_teb[0x30 / 8] == (uint64_t)(uintptr_t)wine_teb && host_teb[0x58 / 8] == 0x2000 + 0x58 / 8 &&
          host_teb[0x60 / 8] == 0x2000 + 0x60 / 8 && host_teb[0x68 / 8] == 0x1000 + 0x68 / 8,
          "enter_guest: Self/TLS/PEB/stack replaced, other fields untouched");
    host_teb[0x58 / 8] = 0x7777;   /* guest code grows its TLS array */
    uwph_teb_leave_guest(host_teb, &s);
    CHECK(host_teb[0x30 / 8] == 0x1000 + 0x30 / 8 && host_teb[0x58 / 8] == 0x1000 + 0x58 / 8 && s.guest[3] == 0x7777,
          "leave_guest: host values back, guest changes kept for next entry");
    CHECK(uwph_arch_prctl(UWPH_ARCH_SET_GS, (uint64_t)(uintptr_t)wine_teb) == 0, "arch_prctl(ARCH_SET_GS) recorded (GS not moved)");
}

/* ---- misc portable ---- */
static void test_misc(void)
{
    struct uwph_utsname u;
    char host[256], *e;
    long tid = unixside_syscall_gettid();

    CHECK(unixside_roundtrip("get_thread_info") == 15, "Unix-side stand-in: request/reply over uwph pipe");
    CHECK(tid == uwph_gettid() && tid > 0, "syscall(SYS_gettid) -> %ld", tid);
    CHECK(uwph_syscall(9999) == -1 && uwph_errno == UWPH_ENOSYS, "unknown syscall -> ENOSYS");
    CHECK(uwph_sysconf(UWPH_SC_PAGESIZE) == 4096, "sysconf(_SC_PAGESIZE)");
    CHECK(uwph_uname(&u) == 0 && !strcmp(u.machine, "x86_64"), "uname: %s %s", u.sysname, u.machine);
    uwph_setenv("WINEPREFIX", "/home/xbox/.wine", 1);
    CHECK((e = uwph_getenv("WINEPREFIX")) && !strcmp(e, "/home/xbox/.wine"), "env table: WINEPREFIX=%s", e ? e : "(null)");
    CHECK(uwph_fs_translate("/app/lib/../lib/wine/ntdll.so", host, sizeof(host)) == UWPH_MOUNT_RO,
          "path /app/... -> install dir (ro): %s", host);
    CHECK(uwph_fs_translate("/home/xbox/.wine/../../../../etc", host, sizeof(host)) == 0, ".. clamps at / -> %s", host);
    CHECK(uwph_fork() == -1 && uwph_errno == UWPH_ENOSYS, "fork -> ENOSYS (single-process host)");
    CHECK(!strcmp(uwph_strerror(UWPH_ENOSYS), "Function not implemented (uwph)"), "strerror uses Linux numbering");
    uwph_fprintf(uwph_stderr, "     (uwph stdio: fprintf to fd 2 works)\n");
}

/* ---- host-dependent: ENOSYS on the stub, real on win32 ---- */
#ifdef UWPH_BACKEND_WIN32
static volatile int futex_word;
static volatile int woke;

static void *futex_waiter(void *arg)
{
    long r = uwph_futex((int *)&futex_word, UWPH_FUTEX_WAIT | UWPH_FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    (void)arg;
    woke = (int)r == 0 ? 1 : -1;
    return NULL;
}

static void *stack_probe(void *arg)
{
    char here;
    /* Record the numeric address while the local is alive. */
    *(uintptr_t *)arg = (uintptr_t)&here;
    return NULL;
}

static int usr1_count;
static void on_usr1(int sig) {
    if (sig == UWPH_SIGUSR1) usr1_count++;
    uwph_errno = UWPH_EBADF; /* handler must not clobber futex errno */
}

struct signal_case { int mode, ready, go, done, result, error, count, after; };
static void *sig_waiter(void *arg)
{
    struct signal_case *c = arg;
    uwph_sigset_t mask;
    struct uwph_timespec timeout = { 2, 0 };
    uwph_sigemptyset(&mask);
    uwph_sigaddset(&mask, UWPH_SIGUSR1);
    if (c->mode >= 2) uwph_pthread_sigmask(UWPH_SIG_BLOCK, &mask, NULL);
    __atomic_store_n(&c->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&c->go, __ATOMIC_ACQUIRE)) SwitchToThread();
    uwph_errno = 123;
    c->result = (int)uwph_futex((int *)&futex_word, UWPH_FUTEX_WAIT, 7, &timeout, NULL, 0);
    c->error = uwph_errno; /* save before any other handler activity */
    c->count = usr1_count;
    if (c->mode >= 2) uwph_pthread_sigmask(UWPH_SIG_UNBLOCK, &mask, NULL);
    c->after = usr1_count;
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int await_atomic(volatile int *p, int nonzero)
{
    uint64_t end = GetTickCount64() + 3000;
    while (!!__atomic_load_n(p, __ATOMIC_ACQUIRE) != nonzero) {
        if (GetTickCount64() >= end) return 0;
        SwitchToThread();
    }
    return 1;
}

static void test_futex_signals(void)
{
    uwph_signal(UWPH_SIGUSR1, (void *)on_usr1);
    for (int repeat = 0; repeat < 20; repeat++) for (int mode = 0; mode < 5; mode++) {
        struct signal_case c = { .mode = mode };
        uwph_pthread_t t;
        int kick = 0;
        usr1_count = 0;
        __atomic_store_n(&futex_word, 7, __ATOMIC_RELEASE);
        CHECK(uwph_pthread_create(&t, NULL, sig_waiter, &c) == 0, "futex case create %d/%d", repeat, mode);
        if (!await_atomic(&c.ready, 1)) { CHECK(0, "ready deadline"); return; }
        if (mode == 0 || mode == 2) kick = uwph_tgkill(uwph_getpid(), t->tid, UWPH_SIGUSR1);
        __atomic_store_n(&c.go, 1, __ATOMIC_RELEASE);
        if (mode != 0) {
            uint64_t end = GetTickCount64() + 3000;
            while (!__atomic_load_n(&t->wait_addr, __ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&c.done, __ATOMIC_ACQUIRE) && GetTickCount64() < end) SwitchToThread();
            CHECK(__atomic_load_n(&t->wait_addr, __ATOMIC_ACQUIRE) != NULL, "waiter published %d/%d", repeat, mode);
            if (mode == 1 || mode == 3) kick = uwph_tgkill(uwph_getpid(), t->tid, UWPH_SIGUSR1);
            if (mode >= 2) {
                CHECK(uwph_futex((int *)&futex_word, UWPH_FUTEX_WAKE, 0, NULL, NULL, 0) == 0,
                      "wake zero leaves waiter queued");
                CHECK(uwph_futex((int *)&futex_word, UWPH_FUTEX_WAKE, 1, NULL, NULL, 0) == 1,
                      "ordinary wake claims equal-word waiter");
            }
        }
        WaitForSingleObjectEx(t->handle, INFINITE, FALSE);
        CHECK(uwph_tgkill(uwph_getpid(), t->tid, UWPH_SIGUSR1) == -1 && uwph_errno == UWPH_ESRCH,
              "exited waiter no longer signal eligible");
        uwph_pthread_join(t, NULL);
        CHECK(kick == 0 && (mode < 2 ? c.result == -1 && c.error == UWPH_EINTR && c.count == 1 && c.after == 1 :
              c.result == 0 && c.error == 123 && c.count == 0 && c.after == (mode == 4 ? 0 : 1)),
              "futex SIGUSR1: %s repeat=%d mode=%d r=%d errno=%d handlers=%d unmask=%d",
              (mode < 2 ? c.result == -1 && c.error == UWPH_EINTR && c.count == 1 && c.after == 1 :
               c.result == 0 && c.error == 123 && c.count == 0 && c.after == (mode == 4 ? 0 : 1)) ? "PASS" : "FAIL",
              repeat, mode, c.result, c.error, c.count, c.after);
    }
}

#endif

#ifdef UWPH_BACKEND_WIN32
static void test_file(void)
{
    char host[1024] = "", cwd_buf[512] = "", buf[64];
    struct uwph_stat st = {0};
    int translated = uwph_fs_translate("/smoke-input.txt", host, sizeof(host));
    DWORD path_error;
    DWORD attrs;
    int fd, open_error;
    DWORD last_error;
    uwph_getcwd(cwd_buf, sizeof(cwd_buf));
    printf("File fixture: uwph CWD=%s translated=%s mount=%d\n", cwd_buf, host, translated);
    CHECK(translated >= 0, "fixture path translation");
    /* Controlled stock Win32 path check only; never substitute for uwph_open. */
    SetLastError(0);
    attrs = GetFileAttributesA(host);
    path_error = GetLastError();
    CHECK(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY),
          "stock Wine path control: GetFileAttributesA path=%s error=%lu", host, (unsigned long)path_error);
    uwph_errno = 0;
    SetLastError(0);
    fd = uwph_open("/smoke-input.txt", UWPH_O_RDONLY);
    last_error = GetLastError();
    open_error = uwph_errno;
    printf("CreateFile2 immediate: path=%s fd=%d GetLastError=%lu uwph_errno=%d\n",
           host, fd, (unsigned long)last_error, open_error);
    CHECK(fd >= 0, "open(\"/smoke-input.txt\") via CreateFile2 -> fd %d", fd);
    if (fd >= 0) {
        uwph_ssize_t n = uwph_read(fd, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = 0;
        CHECK(n > 0 && !strncmp(buf, "uwph smoke input", 16), "read: \"%.16s\"", buf);
        CHECK(uwph_fstat(fd, &st) == 0 && (st.st_mode & UWPH_S_IFMT) == UWPH_S_IFREG && st.st_size == n,
              "fstat: regular, %lld bytes", (long long)st.st_size);
        CHECK(uwph_lseek(fd, 5, UWPH_SEEK_SET) == 5 && uwph_read(fd, buf, 5) == 5 && !memcmp(buf, "smoke", 5),
              "lseek + read");
        CHECK(uwph_close(fd) == 0, "file close");
    } else {
        printf("SKIP dependent file assertions: read/content, fstat, lseek/read, close (open failed)\n");
    }
}
#endif

static void test_host(void)
{
    struct uwph_timespec ts;
    void *p = uwph_mmap(NULL, 3 * 4096, UWPH_PROT_READ | UWPH_PROT_WRITE, UWPH_MAP_PRIVATE | UWPH_MAP_ANONYMOUS, -1, 0);
    int clock_rc = uwph_clock_gettime(UWPH_CLOCK_MONOTONIC, &ts);
    int fd = WIN ? -1 : uwph_open("/smoke-input.txt", UWPH_O_RDONLY);

    if (!WIN) {
        CHECK(p == UWPH_MAP_FAILED && uwph_errno == UWPH_ENOSYS, "stub: mmap -> ENOSYS");
        CHECK(clock_rc == -1, "stub: clock_gettime -> ENOSYS");
        CHECK(fd == -1 && uwph_errno == UWPH_ENOSYS, "stub: open -> ENOSYS");
        CHECK(uwph_futex(NULL, 0, 0, NULL, NULL, 0) == -1 && uwph_errno == UWPH_ENOSYS, "stub: futex -> ENOSYS");
        return;
    }
#ifdef UWPH_BACKEND_WIN32
    {
#ifdef UWPH_DESKTOP_SIGNAL_TEST
        static const unsigned char ret42[] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 };   /* mov eax,42; ret */
        static const unsigned char ret7[]  = { 0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3 };
        unsigned char *rwx;
#endif
        char buf[64];
        uwph_pthread_t t;
        uwph_pthread_attr_t attr;
        void *stk;

        CHECK(p != UWPH_MAP_FAILED, "mmap anon RW -> %p", p);
        CHECK(clock_rc == 0 && ts.tv_sec >= 0, "clock_gettime(MONOTONIC) -> %lld.%09lld", (long long)ts.tv_sec, (long long)ts.tv_nsec);
        CHECK(uwph_munmap(p, 3 * 4096) == 0, "munmap");

#ifdef UWPH_DESKTOP_SIGNAL_TEST
        /* Explicit desktop-only exception execution test. */
        rwx = uwph_mmap(NULL, 4096, UWPH_PROT_READ | UWPH_PROT_WRITE | UWPH_PROT_EXEC,
                        UWPH_MAP_PRIVATE | UWPH_MAP_ANONYMOUS, -1, 0);
        CHECK(rwx != UWPH_MAP_FAILED, "mmap RWX (tracked as W^X-emulated) -> %p", (void *)rwx);
        memcpy(rwx, ret42, sizeof(ret42));
        CHECK(((int (*)(void))rwx)() == 42, "exec fault -> page flipped to RX -> code ran, returned 42");
        memcpy(rwx, ret7, sizeof(ret7));
        CHECK(((int (*)(void))rwx)() == 7, "write fault -> RW, patched, exec fault -> RX again -> 7");
        uwph_munmap(rwx, 4096);
#else
        printf("SKIP live exception execution: unsupported UWP exception hook disabled\n");
#endif

        /* futex across threads */
        futex_word = 0;
        CHECK(uwph_pthread_create(&t, NULL, futex_waiter, NULL) == 0, "pthread_create");
        uwph_usleep(50000);
        futex_word = 1;
        uwph_futex((int *)&futex_word, UWPH_FUTEX_WAKE, 1, NULL, NULL, 0);
        uwph_pthread_join(t, NULL);
        CHECK(woke == 1, "futex WAIT/WAKE between threads (WaitOnAddress)");

        /* thread on a caller-provided stack, as Wine's thread.c does */
        stk = uwph_mmap(NULL, 256 * 1024, UWPH_PROT_READ | UWPH_PROT_WRITE, UWPH_MAP_PRIVATE | UWPH_MAP_ANONYMOUS, -1, 0);
        uwph_pthread_attr_init(&attr);
        uwph_pthread_attr_setstack(&attr, stk, 256 * 1024);
        uintptr_t stack_addr = 0;
        uwph_pthread_create(&t, &attr, stack_probe, &stack_addr);
        uwph_pthread_join(t, NULL);
        CHECK(stack_addr > (uintptr_t)stk && stack_addr < (uintptr_t)stk + 256 * 1024,
              "pthread_attr_setstack honoured: thread ran at %p inside [%p, +256K)", (void *)stack_addr, stk);

        test_futex_signals();

        test_file();
        CHECK(uwph_sysconf(UWPH_SC_NPROCESSORS_ONLN) > 0, "sysconf(NPROCESSORS_ONLN) = %ld", uwph_sysconf(UWPH_SC_NPROCESSORS_ONLN));
        CHECK(uwph_getrandom(buf, 16, 0) == 16, "getrandom (BCryptGenRandom)");
    }
#endif
}

static void print_line(const char *l) { fputs(l, stdout); }

#ifdef UWPH_BACKEND_WIN32
/* Desktop standin: every network operation below goes through uwph. */
static void *inet_delayed_send(void *arg)
{
    Sleep(40);
    uwph_send(*(int *)arg, "later", 5, 0);
    return NULL;
}
static void test_inet(void)
{
    struct { uint16_t family, port; unsigned char ip[4]; unsigned char pad[8]; } a = {UWPH_AF_INET, 0, {127,0,0,1}, {0}};
    struct uwph_sockaddr *addr = (struct uwph_sockaddr *)&a;
    uwph_socklen_t len = sizeof(a);
    struct uwph_addrinfo *ai = NULL;
    struct uwph_pollfd pollfd;
    char b[16] = {0};
    int listener = uwph_socket(UWPH_AF_INET, UWPH_SOCK_STREAM, 0), client, peer, udp, sender;
    int ok = listener >= 0 && uwph_bind(listener, addr, sizeof(a)) == 0 &&
        uwph_getsockname(listener, addr, &len) == 0 && a.port != 0 && uwph_listen(listener, 1) == 0;
    client = uwph_socket(UWPH_AF_INET, UWPH_SOCK_STREAM, 0);
    ok = ok && client >= 0 && uwph_connect(client, addr, sizeof(a)) == 0;
    CHECK(ok && uwph_accept(listener, addr, NULL) == -1 && uwph_errno == UWPH_EFAULT,
          "inet_accept_missing_len EFAULT preserves pending connection");
    /* A bad address pair must not consume the queued connection. */
    CHECK(uwph_fcntl(listener, UWPH_F_SETFL, UWPH_O_NONBLOCK) == 0,
          "inet_accept_retry_nonblocking");
    peer = ok ? uwph_accept(listener, NULL, NULL) : -1;
    ok = ok && peer >= 0 && uwph_send(client, "echo", 4, 0) == 4;
    pollfd = (struct uwph_pollfd){peer, UWPH_POLLIN, 0};
    ok = ok && uwph_poll(&pollfd, 1, 1000) == 1 && (pollfd.revents & UWPH_POLLIN) &&
        uwph_recv(peer, b, sizeof(b), 0) == 4 && uwph_send(peer, b, 4, 0) == 4 &&
        uwph_recv(client, b, sizeof(b), 0) == 4 && !memcmp(b, "echo", 4);
    CHECK(ok, "inet_tcp_echo %s", ok ? "PASS" : "FAIL");
    {
        uwph_host_thread *t = uwph_host_thread_start(inet_delayed_send, &client);
        pollfd = (struct uwph_pollfd){peer, UWPH_POLLIN, 0};
        CHECK(t && uwph_poll(&pollfd, 1, 1000) == 1 && uwph_recv(peer, b, sizeof(b), 0) == 5,
              "inet_poll_delayed PASS");
        if (t) uwph_host_thread_join(t);
    }
    len = sizeof(a);
    CHECK(uwph_getpeername(client, addr, &len) == 0, "inet_getpeername");
    { int val = 1; len = sizeof(val);
      CHECK(uwph_setsockopt(client, UWPH_SOL_SOCKET, UWPH_SO_REUSEADDR, &val, len) == 0 &&
            uwph_getsockopt(client, UWPH_SOL_SOCKET, UWPH_SO_TYPE, &val, &len) == 0 && val == UWPH_SOCK_STREAM &&
            uwph_getsockopt(client, UWPH_SOL_SOCKET, UWPH_SO_REUSEADDR, &val, &len) == 0 && val == 1 &&
            uwph_setsockopt(client, 6 /* IPPROTO_TCP */, 1 /* TCP_NODELAY */, &val, len) == 0, "inet_sockopts"); }
    CHECK(uwph_shutdown(client, UWPH_SHUT_WR) == 0, "inet_shutdown");
    uwph_close(peer); uwph_close(client); uwph_close(listener);
    a.port = 0; len = sizeof(a);
    udp = uwph_socket(UWPH_AF_INET, UWPH_SOCK_DGRAM | UWPH_SOCK_NONBLOCK, 0);
    sender = uwph_socket(UWPH_AF_INET, UWPH_SOCK_DGRAM, 0);
    CHECK(uwph_recv(udp, b, sizeof(b), 0) == -1 && uwph_errno == UWPH_EAGAIN, "inet_nonblocking EAGAIN");
    ok = uwph_bind(udp, addr, sizeof(a)) == 0 && uwph_getsockname(udp, addr, &len) == 0 &&
        uwph_sendto(sender, "udp", 3, 0, addr, len) == 3;
    pollfd = (struct uwph_pollfd){udp, UWPH_POLLIN, 0};
    CHECK(ok && uwph_poll(&pollfd, 1, 1000) == 1 &&
          uwph_recvfrom(udp, b, sizeof(b), 0, addr, NULL) == -1 && uwph_errno == UWPH_EFAULT,
          "inet_recvfrom_missing_len EFAULT preserves queued datagram");
    ok = ok && uwph_poll(&pollfd, 1, 1000) == 1 && uwph_recvfrom(udp, b, sizeof(b), 0, addr, &len) == 3 &&
        uwph_sendto(udp, b, 3, 0, addr, len) == 3 && uwph_recv(sender, b, sizeof(b), 0) == 3 && !memcmp(b, "udp", 3);
    CHECK(ok, "inet_udp_echo %s", ok ? "PASS" : "FAIL");
    uwph_close(udp); uwph_close(sender);
    ok = uwph_getaddrinfo("localhost", "80", NULL, &ai) == 0 && ai &&
        (ai->ai_family == UWPH_AF_INET || ai->ai_family == UWPH_AF_INET6);
    CHECK(ok, "inet_getaddrinfo %s", ok ? "PASS" : "FAIL");
    uwph_freeaddrinfo(ai);
    {
        struct uwph_addrinfo hints = {0};
        hints.ai_family = UWPH_AF_INET6;
        hints.ai_socktype = UWPH_SOCK_DGRAM;
        hints.ai_flags = UWPH_AI_NUMERICHOST;
        ai = NULL;
        ok = uwph_getaddrinfo("::1", "0", &hints, &ai) == 0 && ai && ai->ai_addr->sa_family == UWPH_AF_INET6;
        udp = uwph_socket(UWPH_AF_INET6, UWPH_SOCK_DGRAM, 0);
        sender = uwph_socket(UWPH_AF_INET6, UWPH_SOCK_DGRAM, 0);
        if (ok) {
            unsigned char address[28];
            memcpy(address, ai->ai_addr, sizeof(address));
            len = sizeof(address);
            ok = uwph_bind(udp, (struct uwph_sockaddr *)address, len) == 0 &&
                uwph_getsockname(udp, (struct uwph_sockaddr *)address, &len) == 0 &&
                ((struct uwph_sockaddr *)address)->sa_family == UWPH_AF_INET6 &&
                uwph_sendto(sender, "v6", 2, 0, (struct uwph_sockaddr *)address, len) == 2;
            pollfd = (struct uwph_pollfd){udp, UWPH_POLLIN, 0};
            ok = ok && uwph_poll(&pollfd, 1, 1000) == 1 && uwph_recvfrom(udp, b, sizeof(b), 0, (struct uwph_sockaddr *)address, &len) == 2 &&
                uwph_sendto(udp, b, 2, 0, (struct uwph_sockaddr *)address, len) == 2 && uwph_recv(sender, b, sizeof(b), 0) == 2;
        }
        CHECK(ok, "inet_ipv6_echo %s", ok ? "PASS" : "FAIL");
        uwph_close(udp); uwph_close(sender); uwph_freeaddrinfo(ai);
    }
    listener = uwph_socket(UWPH_AF_INET, UWPH_SOCK_STREAM, 0);
    a.port = (uint16_t)((60000 >> 8) | (60000 << 8));
    CHECK(uwph_bind(listener, addr, sizeof(a)) == -1 && uwph_errno == UWPH_EACCES, "inet_bind_60000 EACCES");
    uwph_close(listener);
    {   /* AF_INET socketpair: loopback listener fallback (Winsock has none) */
        int sv[2] = {-1, -1}, nb[2] = {-1, -1}, val = 0;
        unsigned port0, port1;
        uwph_socklen_t vlen = sizeof(val);
        ok = uwph_socketpair(UWPH_AF_INET, UWPH_SOCK_STREAM, 0, sv) == 0 &&
            uwph_send(sv[0], "pair", 4, 0) == 4 && uwph_recv(sv[1], b, sizeof(b), 0) == 4 && !memcmp(b, "pair", 4) &&
            uwph_write(sv[1], "back", 4) == 4 && uwph_read(sv[0], b, sizeof(b)) == 4 && !memcmp(b, "back", 4);
        len = sizeof(a);
        ok = ok && uwph_getsockname(sv[0], addr, &len) == 0 && a.ip[0] == 127;
        port0 = (unsigned)((a.port >> 8) | ((a.port & 0xff) << 8));
        len = sizeof(a);
        ok = ok && uwph_getsockname(sv[1], addr, &len) == 0;
        port1 = (unsigned)((a.port >> 8) | ((a.port & 0xff) << 8));
        ok = ok && port0 && port0 < 57344 && port1 && port1 < 57344 &&
            uwph_getsockopt(sv[0], UWPH_SOL_SOCKET, UWPH_SO_TYPE, &val, &vlen) == 0 && val == UWPH_SOCK_STREAM;
        if (sv[0] >= 0) uwph_close(sv[0]);
        pollfd = (struct uwph_pollfd){sv[1], UWPH_POLLIN, 0};
        ok = ok && uwph_poll(&pollfd, 1, 1000) == 1 && uwph_recv(sv[1], b, sizeof(b), 0) == 0;
        if (sv[1] >= 0) uwph_close(sv[1]);
        ok = ok && uwph_socketpair(UWPH_AF_INET, UWPH_SOCK_STREAM | UWPH_SOCK_NONBLOCK, 0, nb) == 0 &&
            uwph_recv(nb[1], b, sizeof(b), 0) == -1 && uwph_errno == UWPH_EAGAIN;
        if (nb[0] >= 0) { uwph_close(nb[0]); uwph_close(nb[1]); }
        ok = ok && uwph_socketpair(UWPH_AF_INET, UWPH_SOCK_DGRAM, 0, nb) == -1 && uwph_errno == UWPH_EPROTONOSUPPORT &&
            uwph_socketpair(UWPH_AF_INET6, UWPH_SOCK_STREAM, 0, nb) == -1 && uwph_errno == UWPH_EAFNOSUPPORT;
        printf("inet_socketpair ports %u/%u (below 57344)\n", port0, port1);
        CHECK(ok, "inet_socketpair %s", ok ? "PASS" : "FAIL");
    }
}
#endif

int main(int argc, char **argv)
{
    char root[512];
    (void)argc;
    /* "/" -> the directory holding the test binary's input (LocalFolder on the Xbox) */
    snprintf(root, sizeof(root), "%s", argc > 1 ? argv[1] : (WIN ? "x64\\uwp-host\\tests" : "x64/uwp-host/tests"));
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef UWPH_BACKEND_WIN32
    if (argc > 1 && !strcmp(argv[1], "--futex-only")) {
        printf("Desktop cooperative test-only mode: Windows PE -> uwph wait/signal -> stock Wine Win32; no exception hook or live TEB mutation\n");
        test_inet();
        test_futex_signals();
        printf("%d passed, %d failed\n", passes, fails);
        return fails ? 1 : 0;
    }
#endif
#ifdef UWPH_DESKTOP_SIGNAL_TEST
    printf("Desktop exception test-only mode; no live TEB mutation; not UWP acceptance\n");
#endif
    {
        int rc = uwph_init(root, WIN ? "C:\\uwph-install" : "/opt/uwph-install");
        int error = uwph_errno;
        CHECK(rc == 0, "uwph_init root=%s rc=%d uwph_errno=%d", root, rc, error);
        if (rc != 0) return 1;
    }
#ifdef UWPH_BACKEND_WIN32
    {
        char host_cwd[1024] = "";
        GetCurrentDirectoryA(sizeof(host_cwd), host_cwd);
        printf("Windows process CWD=%s fixture root=%s\n", host_cwd, root);
        if (argc > 2 && !strcmp(argv[2], "--file-only")) {
            test_file();
            printf("%d passed, %d failed\n", passes, fails);
            return fails ? 1 : 0;
        }
    }
#endif
    printf("uwph smoke test, backend: %s\n", uwph_backend_name());
    printf("API probe:\n");
    uwph_probe_print(print_line);

#ifdef UWPH_BACKEND_WIN32
    test_inet();
#endif
    test_fds();
    test_scm_rights();
    test_named_socket();
    test_dl();
    test_signals();
    test_wx_policy();
    test_teb_swap();
    test_misc();
    test_host();

    printf("%d passed, %d failed, open fds at exit: %d\n", passes, fails, uwph_fd_count());
    return fails ? 1 : 0;
}
