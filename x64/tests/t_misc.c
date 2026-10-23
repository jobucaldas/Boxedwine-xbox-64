/* t_misc.c - process, signal, file, directory, malloc and environment conformance.
 *
 * Single-threaded, no fork, no networking, no timing-dependent output.
 * The directory section lists the directory that contains argv[0] and counts
 * only t_* names. Natively that is x64/tests/out; under BoxedWine it is /bin,
 * which run-demo.sh fills with copies of the out files. Listing /bin itself
 * would give different counts natively and in the guest.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t hash;
static uint32_t lcg_state = 0x1234567u;

static uint32_t lcg(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}
static void mix8(uint64_t v) {
    hash ^= (uint8_t)v;
    hash *= FNV_PRIME;
}
static void begin(void) { hash = FNV_OFFSET; }

static void install(int sig, void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
}

/* ---- setjmp / longjmp ------------------------------------------------- */

static jmp_buf jb;

static void deep(int n) {
    if (n == 0) longjmp(jb, 7);
    deep(n - 1);
    printf("deep: unreachable\n");
}

static void test_setjmp(void) {
    volatile int counter = 0;
    int r = setjmp(jb);
    if (r == 0) {
        counter = 1;
        deep(5);
        printf("setjmp: not reached\n");
    } else {
        printf("setjmp: longjmp value=%d counter=%d\n", r, counter);
    }
    /* longjmp(env, 0) must deliver 1 */
    r = setjmp(jb);
    if (r == 0) longjmp(jb, 0);
    printf("setjmp: zero becomes %d\n", r);
}

/* ---- signals ---------------------------------------------------------- */

static sigjmp_buf segv_env;
static volatile sig_atomic_t segv_hits;
static volatile sig_atomic_t segv_signo;

static void on_segv(int sig) {
    segv_hits++;
    segv_signo = sig;
    siglongjmp(segv_env, 1);
}

static void test_segv(void) {
    install(SIGSEGV, on_segv);
    volatile int *volatile np = NULL;
    if (sigsetjmp(segv_env, 1) == 0) {
        *np = 42; /* deliberate NULL write */
        printf("segv: no fault\n");
    } else {
        printf("segv: caught signo=%d hits=%d\n", (int)segv_signo, (int)segv_hits);
    }
    /* the handler must be re-armed: a second fault is caught too */
    if (sigsetjmp(segv_env, 1) == 0) {
        *np = 43;
        printf("segv: no fault 2\n");
    } else {
        printf("segv: caught again hits=%d\n", (int)segv_hits);
    }
}

static sigjmp_buf fpe_env;
static volatile sig_atomic_t fpe_hits;

static void on_fpe(int sig) {
    fpe_hits++;
    siglongjmp(fpe_env, sig);
}

static void test_fpe(void) {
    install(SIGFPE, on_fpe);
    volatile int zero = 0, num = 10, res = 0;
    int sig = sigsetjmp(fpe_env, 1);
    if (sig == 0) {
        res = num / zero;
        printf("fpe: no trap %d\n", res);
    } else {
        printf("fpe: integer divide by zero signo=%d hits=%d\n", sig, (int)fpe_hits);
    }
    volatile int mn = INT_MIN, m1 = -1;
    sig = sigsetjmp(fpe_env, 1);
    if (sig == 0) {
        res = mn / m1;
        printf("fpe: INT_MIN/-1 no trap %d\n", res);
    } else {
        printf("fpe: INT_MIN/-1 signo=%d hits=%d\n", sig, (int)fpe_hits);
    }
}

static volatile sig_atomic_t usr1_hits;
static void on_usr1(int sig) { usr1_hits += (sig == SIGUSR1) ? 1 : 100; }

static void test_usr1(void) {
    install(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    printf("usr1: hits=%d after raise\n", (int)usr1_hits);
    raise(SIGUSR1);
    printf("usr1: hits=%d after second raise\n", (int)usr1_hits);

    sigset_t set, old, pend;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR1);
    sigpending(&pend);
    printf("usr1: blocked hits=%d pending=%d\n", (int)usr1_hits, sigismember(&pend, SIGUSR1) ? 1 : 0);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("usr1: unblocked hits=%d\n", (int)usr1_hits);
    sigprocmask(SIG_SETMASK, &old, NULL);
}

/* ---- process and system info ----------------------------------------- */

static void test_process(void) {
    pid_t pid = getpid();
    printf("pid positive=%d ppid positive=%d\n", pid > 0, getppid() > 0);
    char cwd[4096] = "";
    int cwd_ok = getcwd(cwd, sizeof cwd) != NULL;
    printf("getcwd ok=%d absolute=%d\n", cwd_ok, cwd[0] == '/');
    struct utsname u;
    if (uname(&u) == 0)
        printf("uname sysname=%s machine=%s\n", u.sysname, u.machine);
    else
        printf("uname failed errno=%d\n", errno);
}

/* ---- file I/O ---------------------------------------------------------- */

static void test_file(void) {
    const char *path = "/tmp/t_misc_io.dat";
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        printf("open create failed errno=%d\n", errno);
        return;
    }
    unsigned char buf[1000], rd[100];
    for (int i = 0; i < 1000; i++) buf[i] = (unsigned char)(i * 31 + 7);
    ssize_t w = write(fd, buf, sizeof buf);
    off_t pos = lseek(fd, 500, SEEK_SET);
    ssize_t r = read(fd, rd, sizeof rd);
    int same = r == 100 && memcmp(rd, buf + 500, 100) == 0;
    off_t endpos = lseek(fd, 0, SEEK_END);
    struct stat st;
    int fs = fstat(fd, &st);
    printf("write=%zd lseek=%ld read=%zd match=%d end=%ld fstat=%d size=%ld reg=%d\n", w, (long)pos, r, same,
           (long)endpos, fs, (long)st.st_size, S_ISREG(st.st_mode) ? 1 : 0);
    close(fd);

    struct stat st2;
    int s2 = stat(path, &st2);
    printf("stat=%d size=%ld reg=%d\n", s2, (long)st2.st_size, S_ISREG(st2.st_mode) ? 1 : 0);

    fd = open(path, O_WRONLY | O_APPEND);
    ssize_t aw = write(fd, "tail", 4);
    close(fd);
    stat(path, &st2);
    printf("append=%zd size=%ld\n", aw, (long)st2.st_size);

    fd = open(path, O_RDONLY);
    begin();
    for (;;) {
        unsigned char chunk[333];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) mix8(chunk[i]);
    }
    close(fd);
    printf("content hash %016llx\n", (unsigned long long)hash);

    printf("unlink=%d", unlink(path));
    errno = 0;
    int s3 = stat(path, &st2);
    printf(" stat-after=%d errno=%d\n", s3, s3 ? errno : 0);
}

/* ---- directory listing ------------------------------------------------ */

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void test_dir(const char *argv0) {
    char dir[PATH_MAX];
    const char *slash = strrchr(argv0, '/');
    const char *base = slash ? slash + 1 : argv0;
    if (slash) {
        size_t n = (size_t)(slash - argv0);
        if (n == 0) n = 1; /* "/t_misc" -> "/" */
        memcpy(dir, argv0, n);
        dir[n] = 0;
    } else {
        strcpy(dir, ".");
    }
    DIR *d = opendir(dir);
    if (!d) {
        printf("opendir failed errno=%d\n", errno);
        return;
    }
    /* Only t_* names are counted: the guest /bin can also hold stale files from earlier runs,
     * which are not part of this test set. */
    static char *names[4096];
    int count = 0, own = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "t_", 2) != 0) continue;
        if (count < 4096) names[count++] = strdup(de->d_name);
        if (!strcmp(de->d_name, base)) own = 1;
    }
    closedir(d);
    qsort(names, count, sizeof names[0], cmp_names);
    printf("dir sorted t_* count=%d own_binary_listed=%d\n", count, own);
    for (int i = 0; i < count; i++) free(names[i]);

    struct stat st;
    printf("stat /bin dir=%d\n", stat("/bin", &st) == 0 && S_ISDIR(st.st_mode));
}

/* ---- malloc / realloc / free ----------------------------------------- */

static unsigned char pat(unsigned slot, size_t j) { return (unsigned char)(j * 131u + slot * 17u + 3u); }

static void test_malloc(void) {
    enum { NSLOT = 256 };
    static unsigned char *slot[NSLOT];
    static size_t slen[NSLOT];
    int bad = 0;
    for (int step = 0; step < 20000; step++) {
        uint32_t s = lcg();
        unsigned i = s % NSLOT;
        unsigned op = (s >> 8) % 4;
        if (!slot[i] || op == 0) {
            size_t n = 1 + (s >> 12) % 4096;
            free(slot[i]);
            slot[i] = malloc(n);
            if (!slot[i]) { bad++; continue; }
            slen[i] = n;
            for (size_t j = 0; j < n; j++) slot[i][j] = pat(i, j);
        } else if (op == 1) {
            size_t n = 1 + (s >> 12) % 8192;
            unsigned char *p = realloc(slot[i], n);
            if (!p) { bad++; continue; }
            slot[i] = p;
            for (size_t j = slen[i]; j < n; j++) slot[i][j] = pat(i, j);
            slen[i] = n;
        } else if (op == 2) {
            for (size_t j = 0; j < slen[i]; j++)
                if (slot[i][j] != pat(i, j)) bad++;
        } else {
            free(slot[i]);
            slot[i] = NULL;
            slen[i] = 0;
        }
    }
    begin();
    uint64_t total = 0;
    for (unsigned i = 0; i < NSLOT; i++) {
        if (!slot[i]) continue;
        for (size_t j = 0; j < slen[i]; j++) {
            if (slot[i][j] != pat(i, j)) bad++;
            mix8(slot[i][j]);
        }
        total += slen[i];
        free(slot[i]);
    }
    printf("malloc stress bad=%d live_bytes=%llu checksum=%016llx\n", bad, (unsigned long long)total,
           (unsigned long long)hash);

    unsigned char *z = calloc(1000, 4);
    int zeroed = 1;
    for (int i = 0; i < 4000; i++)
        if (z[i]) zeroed = 0;
    free(z);
    unsigned char *big = malloc(4u << 20);
    memset(big, 0xab, 4u << 20);
    int bigok = big[0] == 0xab && big[(4u << 20) - 1] == 0xab;
    free(big);
    void *al = aligned_alloc(4096, 8192);
    printf("calloc zeroed=%d big ok=%d aligned=%d\n", zeroed, bigok, al && ((uintptr_t)al % 4096) == 0);
    free(al);
    printf("free(NULL) ok=%d realloc(NULL,n) ok=%d\n", (free(NULL), 1), realloc(NULL, 16) != NULL);
}

/* ---- environment ------------------------------------------------------ */

static void test_env(void) {
    setenv("BOXEDWINE_T_VAR", "alpha=beta", 1);
    printf("getenv=%s\n", getenv("BOXEDWINE_T_VAR"));
    setenv("BOXEDWINE_T_VAR", "gamma", 0); /* overwrite=0 keeps the old value */
    printf("no-overwrite=%s\n", getenv("BOXEDWINE_T_VAR"));
    setenv("BOXEDWINE_T_VAR", "gamma", 1);
    printf("overwrite=%s\n", getenv("BOXEDWINE_T_VAR"));
    setenv("BOXEDWINE_T_EMPTY", "", 1);
    printf("empty=[%s]\n", getenv("BOXEDWINE_T_EMPTY"));
    unsetenv("BOXEDWINE_T_VAR");
    printf("unset null=%d\n", getenv("BOXEDWINE_T_VAR") == NULL);
    printf("missing null=%d\n", getenv("BOXEDWINE_NOT_SET_XYZ") == NULL);
}

int main(int argc, char **argv) {
    printf("t_misc: process, signal, file and memory conformance\n");
    printf("argc=%d\n", argc);
    test_setjmp();
    test_segv();
    test_fpe();
    test_usr1();
    test_process();
    test_file();
    test_dir(argv[0]);
    test_malloc();
    test_env();
    printf("t_misc done\n");
    return 0;
}
