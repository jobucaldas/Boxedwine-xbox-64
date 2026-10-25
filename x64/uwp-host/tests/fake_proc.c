#define _GNU_SOURCE
#include "uwph/uwph.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
static int snapshot(const char *path, char *buf, size_t size)
{
    int fd = uwph_open(path, UWPH_O_RDONLY);
    assert(fd >= 0);
    int n = uwph_read(fd, buf, size - 1);
    assert(n > 0); buf[n] = 0; assert(!uwph_close(fd)); return n;
}
/* Wine 10.0 dlls/ntdll/unix/process.c fill_vm_counters loop, verbatim
 * (line buffer 256, non-==1 sscanf tests). Returns parsed fields seen. */
static int wine_vm_counters(const char *text, unsigned long long *peak, unsigned long long *size)
{
    FILE *f = fmemopen((void *)text, strlen(text), "r"); assert(f);
    char line[256]; unsigned long value; int seen = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (sscanf(line, "VmPeak: %lu", &value)) { *peak = (unsigned long long)value * 1024; seen++; }
        else if (sscanf(line, "VmSize: %lu", &value)) { *size = (unsigned long long)value * 1024; seen++; }
        else if (sscanf(line, "VmHWM: %lu", &value)) seen++;
        else if (sscanf(line, "VmRSS: %lu", &value)) seen++;
        else if (sscanf(line, "RssAnon: %lu", &value)) seen++;
        else if (sscanf(line, "VmSwap: %lu", &value)) seen++;
    }
    fclose(f); return seen;
}
/* Wine 10.0 dlls/ntdll/unix/system.c meminfo loop, verbatim (line buffer 64). */
static void wine_meminfo(const char *text, unsigned long long *totalram, unsigned long long *freeram,
                         unsigned long long *totalswap)
{
    FILE *fp = fmemopen((void *)text, strlen(text), "r"); assert(fp);
    unsigned long long value, mem_available = 0, freeswap = 0; char line[64];
    *totalram = *freeram = *totalswap = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if(sscanf(line, "MemTotal: %llu kB", &value) == 1) *totalram += value * 1024;
        else if(sscanf(line, "MemFree: %llu kB", &value) == 1) *freeram += value * 1024;
        else if(sscanf(line, "SwapTotal: %llu kB", &value) == 1) *totalswap += value * 1024;
        else if(sscanf(line, "SwapFree: %llu kB", &value) == 1) freeswap += value * 1024;
        else if (sscanf(line, "Buffers: %llu", &value)) *freeram += value * 1024;
        else if (sscanf(line, "Cached: %llu", &value)) *freeram += value * 1024;
        else if (sscanf(line, "MemAvailable: %llu", &value)) mem_available = value * 1024;
    }
    fclose(fp);
    if (mem_available) *freeram = mem_available;
}
/* Wine 10.0 dlls/ntdll/unix/thread.c get_thread_times parser. */
static int wine_thread_times(const char *text, unsigned long *usr, unsigned long *sys)
{
    char buf[512]; snprintf(buf, sizeof(buf), "%s", text);
    const char *pos = strchr(buf, '\n') ? buf : NULL;
    if (pos) pos = strrchr(pos, ')');
    if (pos) pos = strchr(pos + 1, ' ');
    if (pos) pos++;
    for (int i = 0; i < 11 && pos; i++) { pos = strchr(pos + 1, ' '); if (pos) pos++; }
    return pos && sscanf(pos, "%lu %lu", usr, sys) == 2;
}
static atomic_int ready, finish;
static void *worker(void *unused)
{
    atomic_store(&ready, 1);
    while (!atomic_load(&finish)) usleep(1000);
    return NULL;
}
static int tasks(void)
{
    uwph_DIR *d = uwph_opendir("/proc/self/task"); assert(d);
    struct uwph_dirent *e; int n = 0;
    while ((e = uwph_readdir(d))) if (e->d_name[0] != '.') {
        assert(e->d_type == UWPH_DT_DIR); assert(atoi(e->d_name) > 0); n++;
    }
    assert(!uwph_closedir(d)); return n;
}
static void extended(void)
{
    char b[16384], path[128]; unsigned long v; unsigned long long total, freeram, swap, peak = 0, size = 0;
    snapshot("/proc/self/status", b, sizeof(b));
    assert(wine_vm_counters(b, &peak, &size) == 6 && peak >= size);
    char *p = strstr(b, "\nThreads:"); assert(p);
    assert(sscanf(p, "\nThreads: %lu", &v) == 1 && v == (unsigned long)tasks());
    snapshot("/proc/self/stat", b, sizeof(b));
    unsigned long user, sys; assert(wine_thread_times(b, &user, &sys));
    int fields = 3; p = strrchr(b, ')') + 3;
    while ((p = strchr(p, ' '))) { fields++; p++; }
    assert(fields == 52);
    snprintf(path, sizeof(path), "/proc/%d/stat", uwph_getpid());
    snapshot(path, b, sizeof(b)); assert(wine_thread_times(b, &user, &sys));
    snprintf(path, sizeof(path), "/proc/%d/status", uwph_getpid());
    snapshot(path, b, sizeof(b)); assert(wine_vm_counters(b, &peak, &size) == 6);
    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", uwph_getpid(), uwph_gettid());
    snapshot(path, b, sizeof(b)); assert(wine_thread_times(b, &user, &sys));
    assert(atoi(b) == uwph_gettid());
    assert(uwph_open("/proc/self/task/99999999/stat", 0) == -1);
    snapshot("/proc/meminfo", b, sizeof(b));
    wine_meminfo(b, &total, &freeram, &swap);
    assert(total == 1024ULL << 20 && freeram && freeram <= total && !swap);
    setenv("UWPH_MEM_BUDGET_MB", "5120", 1);
    snapshot("/proc/meminfo", b, sizeof(b));
    wine_meminfo(b, &total, &freeram, &swap);
    assert(total == 5120ULL << 20 && freeram && freeram <= total);
    setenv("UWPH_MEM_BUDGET_MB", "invalid", 1);
    assert(uwph_open("/proc/meminfo", 0) == -1 && uwph_errno == UWPH_EINVAL);
    snapshot("/proc/self/status", b, sizeof(b));   /* budget only scopes meminfo */
    unsetenv("UWPH_MEM_BUDGET_MB");
    int n = snapshot("/proc/self/auxv", b, sizeof(b));
    assert(n % (2 * sizeof(uintptr_t)) == 0);
    int page = 0, end = 0;
    for (int i = 0; i < n; i += 2 * sizeof(uintptr_t)) {
        uintptr_t pair[2]; memcpy(pair, b + i, sizeof(pair));
        if (pair[0] == 6) { assert(pair[1] == 4096); page++; }
        if (!pair[0]) { assert(!pair[1] && i + (int)sizeof(pair) == n); end++; }
    }
    assert(page == 1 && end == 1);
    int fd = uwph_open("/proc/meminfo", 0); assert(fd >= 0);
    uwph_DIR *d = uwph_opendir("/proc/self/fd"); assert(d);
    struct uwph_dirent *e; int found = 0;
    while ((e = uwph_readdir(d))) if (e->d_name[0] != '.') {
        assert(e->d_type == UWPH_DT_LNK);
        if (atoi(e->d_name) == fd) found++;
    }
    assert(found == 1); assert(!uwph_closedir(d));
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = uwph_readlink(path, b, sizeof(b)); assert(n > 0);
    assert(!uwph_close(fd)); assert(uwph_readlink(path, b, sizeof(b)) == -1);
    int before = tasks(); uwph_pthread_t t;
    assert(!uwph_pthread_create(&t, NULL, worker, NULL));
    while (!atomic_load(&ready)) usleep(1000);
    assert(tasks() == before + 1);
    atomic_store(&finish, 1); assert(!uwph_pthread_join(t, NULL));
    assert(tasks() == before);
    void *map = uwph_mmap(NULL, 8192, UWPH_PROT_READ | UWPH_PROT_WRITE, UWPH_MAP_PRIVATE | UWPH_MAP_ANONYMOUS, -1, 0);
    assert(map != UWPH_MAP_FAILED);
    snapshot("/proc/self/status", b, sizeof(b));
    assert(sscanf(strstr(b, "VmSize:"), "VmSize: %lu", &v) == 1 && v >= 8);
    assert(!uwph_munmap(map, 4096)); assert(!uwph_munmap((char *)map + 4096, 4096));
    puts("PASS status/stat/fd/auxv/meminfo/task formats, budgets, fd and thread lifecycle, mapping accounting THROUGH uwph");
}
int main(int argc, char **argv)
{
    char b[16384], exe[1024];
    /* Existing host paths must never escape the provider, for either API. */
    assert(uwph_open("/proc", UWPH_O_RDONLY) == -1);
    assert(uwph_open("/proc/version", UWPH_O_RDONLY) == -1);
    assert(uwph_errno == UWPH_ENOENT);
    assert(uwph_fopen("/proc/version", "r") == NULL);
    assert(uwph_errno == UWPH_ENOENT);
    if (argc > 1 && !strcmp(argv[1], "--host-proc")) {
        puts("PASS unsupported host proc paths blocked");
        return 0;
    }
    extended();
    /* A raw call must fail in the strict namespace; provider calls must work. */
    assert(syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY) == -1);
    const char *paths[] = {"/proc/self/maps", "/proc/self/cmdline", "/proc/cpuinfo"};
    for (unsigned i = 0; i < 3; i++) {
        int fd = uwph_open(paths[i], UWPH_O_RDONLY);
        assert(fd >= 0);
        int n = uwph_read(fd, b, sizeof(b)-1);
        assert(n > 0); b[n] = 0;
        if (!i) assert(strstr(b, "r-xp"));
        if (i == 1) assert(b[n-1] == 0);
        if (i == 2) assert(strstr(b, "sse2"));
        assert(uwph_lseek(fd, 0, UWPH_SEEK_SET) == 0);
        assert(uwph_close(fd) == 0);
        assert(uwph_open(paths[i], UWPH_O_WRONLY) == -1);
        uwph_FILE *f = uwph_fopen(paths[i], "r");
        assert(f); assert(uwph_fclose(f) == 0);
    }
    int n = uwph_readlink("/proc/self/exe", exe, sizeof(exe));
    assert(n > 0);
    assert(exe[0] == '/' && n < (int)sizeof(exe));
    exe[n] = 0;
    assert(chdir("/") == 0);
    char resolved[4096], after[1024];
    assert(uwph_realpath("/proc/self/exe", resolved) == resolved);
    assert(!strcmp(resolved, exe));
    assert(uwph_readlink("/proc/self/exe", after, sizeof(after)) == n);
    assert(!memcmp(after, exe, n));
    int executable = uwph_open("/proc/self/exe", UWPH_O_RDONLY);
    assert(executable >= 0);
    assert(uwph_read(executable, b, 4) == 4 && !memcmp(b, "\177ELF", 4));
    assert(uwph_close(executable) == 0);
    char shortlink[2] = {0};
    assert(uwph_readlink("/proc/self/exe", shortlink, 1) == 1);
    assert(shortlink[0] == exe[0] && shortlink[1] == 0);
    assert(uwph_open("/proc/unsupported", UWPH_O_RDONLY) == -1);
    puts("PASS fake proc through uwph; raw host /proc unavailable");
}
