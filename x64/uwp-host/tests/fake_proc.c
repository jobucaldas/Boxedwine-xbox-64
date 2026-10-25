#define _GNU_SOURCE
#include "uwph/uwph.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
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
