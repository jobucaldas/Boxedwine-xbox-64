#define _GNU_SOURCE
#include "internal.h"
#include "fake_proc.h"
#include <stdio.h>
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
#include <link.h>
#include <sys/auxv.h>
#include <unistd.h>
#include <limits.h>
/* Capture invocation identity before application code can change cwd.
 * No dependency on procfs, including when AT_EXECFN is relative. */
static char invocation_exe[PATH_MAX];
/* Preload constructors (including the hosted server) may query this identity. */
__attribute__((constructor(101))) static void capture_invocation_exe(void)
{
    const char *exe = (const char *)getauxval(AT_EXECFN);
    if (!exe) return;
    size_t prefix = 0;
    if (*exe != '/') {
        if (!getcwd(invocation_exe, sizeof(invocation_exe))) return;
        prefix = strlen(invocation_exe);
        if (prefix + 1 >= sizeof(invocation_exe)) { invocation_exe[0] = 0; return; }
        invocation_exe[prefix++] = '/';
    }
    if (strlen(exe) >= sizeof(invocation_exe) - prefix) { invocation_exe[0] = 0; return; }
    strcpy(invocation_exe + prefix, exe);
}
#endif

int uwph_fake_proc_exe(char *buf, size_t size)
{
#ifdef UWPH_BACKEND_WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    if (!n || n >= size) return UWPH_FAIL(UWPH_ENAMETOOLONG);
    return (int)n;
#elif defined(UWPH_BACKEND_LINUX_PASSTHROUGH)
    const char *exe = invocation_exe;
    if (!*exe) return UWPH_FAIL(UWPH_ENOENT);
    size_t n = strlen(exe);
    if (n >= size) return UWPH_FAIL(UWPH_ENAMETOOLONG);
    memcpy(buf, exe, n + 1);
    return (int)n;
#else
    return UWPH_ENOSYS_INT();
#endif
}

/* Only descriptors/mappings observed by uwph are represented. No OS scan. */
#define PROC_SLOTS 4096
static uwph_lock_t proc_lock = UWPH_LOCK_INIT;
static struct { int used, fd; char path[1024]; } proc_fds[PROC_SLOTS];
static struct { uintptr_t start, end; } proc_maps[PROC_SLOTS];
static uint64_t peak_bytes;
void uwph_proc_fd_open(int fd, const char *path)
{
    if (fd < 0) return;
    uwph_lock(&proc_lock);
    int slot = -1;
    for (int i = 0; i < PROC_SLOTS; i++) {
        if (proc_fds[i].used && proc_fds[i].fd == fd) { slot = i; break; }
        if (!proc_fds[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) {
        proc_fds[slot].used = 1; proc_fds[slot].fd = fd;
        snprintf(proc_fds[slot].path, sizeof(proc_fds[slot].path), "%s", path);
    }
    uwph_unlock(&proc_lock);
}
void uwph_proc_fd_close(int fd)
{
    uwph_lock(&proc_lock);
    for (int i = 0; i < PROC_SLOTS; i++)
        if (proc_fds[i].used && proc_fds[i].fd == fd) proc_fds[i].used = 0;
    uwph_unlock(&proc_lock);
}
static uint64_t mapping_bytes(void)
{
    uint64_t n = 0;
    for (int i = 0; i < PROC_SLOTS; i++) n += proc_maps[i].end - proc_maps[i].start;
    return n;
}
void uwph_proc_mapping(void *addr, size_t len, int mapped)
{
    uintptr_t a = (uintptr_t)addr, b = a + ((len + 4095) & ~(size_t)4095);
    if (!len || b < a) return;
    uwph_lock(&proc_lock);
    for (int i = 0; i < PROC_SLOTS; i++) {
        uintptr_t x = proc_maps[i].start, y = proc_maps[i].end;
        if (y <= a || x >= b) continue;
        if (x < a && y > b) {
            for (int j = 0; j < PROC_SLOTS; j++) if (!proc_maps[j].end) {
                proc_maps[j].start = b; proc_maps[j].end = y; break;
            }
            proc_maps[i].end = a;
        } else if (x < a) proc_maps[i].end = a;
        else if (y > b) proc_maps[i].start = b;
        else proc_maps[i].start = proc_maps[i].end = 0;
    }
    if (mapped) for (int i = 0; i < PROC_SLOTS; i++) if (!proc_maps[i].end) {
        proc_maps[i].start = a; proc_maps[i].end = b; break;
    }
    uint64_t n = mapping_bytes();
    if (n > peak_bytes) peak_bytes = n;
    uwph_unlock(&proc_lock);
}
static int fd_ids(int *ids)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    int n = 0;
    uwph_lock(&proc_lock);
    for (int i = 0; i < PROC_SLOTS; i++) if (proc_fds[i].used) ids[n++] = proc_fds[i].fd;
    uwph_unlock(&proc_lock);
    return n;
#else
    return uwph_fd_ids(ids, PROC_SLOTS);
#endif
}
int uwph_fake_proc_dir(const char *path)
{
    return !strcmp(path, "/proc/self/fd") || !strcmp(path, "/proc/self/task");
}
int uwph_fake_proc_link(const char *path, char *buf, size_t size)
{
    int fd, used = 0; char tail;
    if (sscanf(path, "/proc/self/fd/%d%c", &fd, &tail) != 1) return UWPH_FAIL(UWPH_ENOENT);
    char target[1024];
    uwph_lock(&proc_lock);
    for (int i = 0; i < PROC_SLOTS; i++) if (proc_fds[i].used && proc_fds[i].fd == fd) {
        strcpy(target, proc_fds[i].path); used = 1; break;
    }
    uwph_unlock(&proc_lock);
#ifndef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (!used) {
        int ids[PROC_SLOTS], n = fd_ids(ids);
        for (int i = 0; i < n; i++) if (ids[i] == fd) {
            snprintf(target, sizeof(target), "uwph:fd/%d", fd); used = 1; break;
        }
    }
#endif
    if (!used) return UWPH_FAIL(UWPH_ENOENT);
    size_t n = strlen(target); if (n > size) n = size;
    memcpy(buf, target, n); return (int)n;
}
struct uwph_dir {
    struct uwph_dir *next;
    int ids[PROC_SLOTS], count, at, type;
    struct uwph_dirent entry;
};
static struct uwph_dir *directories;
int uwph_fake_dir_owned(const void *dir)
{
    int found = 0;
    uwph_lock(&proc_lock);
    for (struct uwph_dir *d = directories; d; d = d->next) if (d == dir) { found = 1; break; }
    uwph_unlock(&proc_lock); return found;
}
uwph_DIR *uwph_fake_opendir(const char *path)
{
    if (!uwph_fake_proc_dir(path)) return UWPH_FAILP(UWPH_ENOENT);
    struct uwph_dir *d = calloc(1, sizeof(*d));
    if (!d) return UWPH_FAILP(UWPH_ENOMEM);
    d->type = !strcmp(path, "/proc/self/task") ? UWPH_DT_DIR : UWPH_DT_LNK;
    d->count = d->type == UWPH_DT_DIR ? uwph_thread_ids(d->ids, PROC_SLOTS) : fd_ids(d->ids);
    uwph_lock(&proc_lock); d->next = directories; directories = d; uwph_unlock(&proc_lock);
    return d;
}
struct uwph_dirent *uwph_fake_readdir(uwph_DIR *d)
{
    if (!uwph_fake_dir_owned(d)) return UWPH_FAILP(UWPH_EBADF);
    if (d->at >= d->count + 2) return NULL;
    memset(&d->entry, 0, sizeof(d->entry));
    d->entry.d_ino = d->at + 1; d->entry.d_off = d->at + 1;
    d->entry.d_reclen = sizeof(d->entry);
    d->entry.d_type = d->at < 2 ? UWPH_DT_DIR : d->type;
    if (d->at < 2) strcpy(d->entry.d_name, d->at ? ".." : ".");
    else snprintf(d->entry.d_name, sizeof(d->entry.d_name), "%d", d->ids[d->at - 2]);
    d->at++; return &d->entry;
}
int uwph_fake_closedir(uwph_DIR *d)
{
    uwph_lock(&proc_lock);
    struct uwph_dir **p = &directories;
    while (*p && *p != d) p = &(*p)->next;
    if (!*p) { uwph_unlock(&proc_lock); return UWPH_FAIL(UWPH_EBADF); }
    *p = d->next; uwph_unlock(&proc_lock); free(d); return 0;
}

struct snapshot { char *data; size_t length, capacity; int failed; };
static void append(struct snapshot *s, const char *line)
{
    size_t n = strlen(line);
    if (s->failed) return;
    if (s->length + n + 1 > s->capacity) {
        size_t cap = (s->length + n + 1) * 2;
        char *p = realloc(s->data, cap);
        if (!p) { s->failed = 1; return; }
        s->data = p; s->capacity = cap;
    }
    memcpy(s->data + s->length, line, n + 1); s->length += n;
}
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
/* ELF load segments only: anonymous/PE mappings are not yet enumerated.
 * Never substitute a saved host maps file or open real /proc. */
static int elf_maps(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct snapshot *s = arg;
    char line[1400];
    for (unsigned i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        unsigned long long a = (info->dlpi_addr + p->p_vaddr) & ~4095ULL;
        unsigned long long b = (info->dlpi_addr + p->p_vaddr + p->p_memsz + 4095) & ~4095ULL;
        snprintf(line, sizeof(line), "%012llx-%012llx %c%c%cp %08llx 00:00 0 %s\n",
                 a, b, p->p_flags & PF_R ? 'r' : '-', p->p_flags & PF_W ? 'w' : '-',
                 p->p_flags & PF_X ? 'x' : '-', (unsigned long long)p->p_offset & ~4095ULL,
                 info->dlpi_name);
        append(s, line);
    }
    return 0;
}
#endif
int uwph_fake_proc_snapshot(const char *path, char **data, size_t *length)
{
    struct snapshot s = {0};
    char exe[1024];
    if (strncmp(path, "/proc/", 6)) return 0;
    char normalized[128];
    unsigned pid; int consumed = 0;
    if (sscanf(path, "/proc/%u/%n", &pid, &consumed) == 1 && consumed) {
        if (pid != (unsigned)uwph_getpid()) return UWPH_FAIL(UWPH_ENOENT);
        snprintf(normalized, sizeof(normalized), "/proc/self/%s", path + consumed);
        path = normalized;
    }
    int ids[PROC_SLOTS], threads = uwph_thread_ids(ids, PROC_SLOTS);
    int tid = 0, task_end = 0;
    sscanf(path, "/proc/self/task/%d/stat%n", &tid, &task_end);
    int task_stat = task_end && !path[task_end];
    if (task_stat) {
        int found = 0;
        for (int i = 0; i < threads; i++) found |= ids[i] == tid;
        if (!found) return UWPH_FAIL(UWPH_ENOENT);
    }
    uint64_t bytes, peak;
    uwph_lock(&proc_lock); bytes = mapping_bytes(); peak = peak_bytes; uwph_unlock(&proc_lock);
    char line[2048];
    if (!strcmp(path, "/proc/self/status")) {
        snprintf(line, sizeof(line), "Name:\tuwph\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t0\nTracerPid:\t0\nThreads:\t%d\nFDSize:\t%d\nVmPeak:\t%llu kB\nVmSize:\t%llu kB\nVmHWM:\t0 kB\nVmRSS:\t0 kB\nRssAnon:\t0 kB\nVmSwap:\t0 kB\n",
                 uwph_getpid(), uwph_getpid(), threads, fd_ids(ids),
                 (unsigned long long)(peak / 1024), (unsigned long long)(bytes / 1024));
        append(&s, line);
    } else if (!strcmp(path, "/proc/self/stat") || task_stat) {
        /* 52 Linux fields; unknown CPU/residency counters are zero. */
        snprintf(line, sizeof(line), "%d (uwph) R", task_stat ? tid : uwph_getpid());
        append(&s, line);
        for (int field = 4; field <= 52; field++) {
            uint64_t value = field == 20 ? (uint64_t)threads : field == 23 ? bytes : 0;
            snprintf(line, sizeof(line), " %llu", (unsigned long long)value); append(&s, line);
        }
        append(&s, "\n");
    } else if (!strcmp(path, "/proc/meminfo")) {
        /* UWP memory budget in MB: App 1024 (default) or Game 5120. */
        const char *budget_env = getenv("UWPH_MEM_BUDGET_MB");
        if (budget_env && strcmp(budget_env, "1024") && strcmp(budget_env, "5120"))
            return UWPH_FAIL(UWPH_EINVAL);
        uint64_t budget = (budget_env && !strcmp(budget_env, "5120") ? 5120ULL : 1024ULL) * 1024;
        uint64_t free_kb = bytes / 1024 < budget ? budget - bytes / 1024 : 0;
        snprintf(line, sizeof(line), "MemTotal: %llu kB\nMemFree: %llu kB\nMemAvailable: %llu kB\nBuffers: 0 kB\nCached: 0 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n",
                 (unsigned long long)budget, (unsigned long long)free_kb, (unsigned long long)free_kb);
        append(&s, line);
    } else if (!strcmp(path, "/proc/self/auxv")) {
        /* x86-64 ELF auxv pairs, terminated by AT_NULL. In-process startup
         * values on Linux; documented target constants on UWP. */
        uintptr_t aux[][2] = {{6, 4096}, {17, 100}, {16, 0}, {26, 0}, {0, 0}};
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
        aux[2][1] = getauxval(AT_HWCAP); aux[3][1] = getauxval(AT_HWCAP2);
#endif
        s.length = sizeof(aux); s.data = malloc(s.length);
        if (!s.data) return UWPH_FAIL(UWPH_ENOMEM);
        memcpy(s.data, aux, s.length);
    } else if (uwph_fake_proc_dir(path)) return UWPH_FAIL(UWPH_EISDIR);
    else if (!strcmp(path, "/proc/self/cmdline")) {
        /* Explicit host invocation identity; no claim to full Linux argv. */
        int n = uwph_fake_proc_exe(exe, sizeof(exe));
        if (n < 0) return -1;
        append(&s, exe); s.length++; /* include the NUL */
    } else if (!strcmp(path, "/proc/cpuinfo")) {
        append(&s, "processor\t: 0\n"
                   "model name\t: uwph x86-64 compatibility host\n"
                   "flags\t\t: fpu sse sse2\n\n");
    } else if (!strcmp(path, "/proc/self/maps")) {
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
        dl_iterate_phdr(elf_maps, &s);
#elif defined(UWPH_BACKEND_WIN32)
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t at = 0;
        char line[160];
        while (VirtualQuery((void *)at, &mbi, sizeof(mbi))) {
            uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (end <= at) break;
            if (mbi.State == MEM_COMMIT) {
                DWORD p = mbi.Protect & 0xff;
                int r = p != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
                int w = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
                int x = p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
                snprintf(line, sizeof(line), "%012llx-%012llx %c%c%cp 00000000 00:00 0\n",
                         (unsigned long long)(uintptr_t)mbi.BaseAddress, (unsigned long long)end,
                         r ? 'r' : '-', w ? 'w' : '-', x ? 'x' : '-');
                append(&s, line);
            }
            at = end;
        }
#else
        return UWPH_ENOSYS_INT();
#endif
    } else return UWPH_FAIL(UWPH_ENOENT);
    if (s.failed) { free(s.data); return UWPH_FAIL(UWPH_ENOMEM); }
    if (!s.data) { s.data = calloc(1, 1); if (!s.data) return UWPH_FAIL(UWPH_ENOMEM); }
    *data = s.data; *length = s.length;
    return 1;
}
