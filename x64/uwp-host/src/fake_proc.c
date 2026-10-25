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
__attribute__((constructor)) static void capture_invocation_exe(void)
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
    if (!strcmp(path, "/proc/self/cmdline")) {
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
