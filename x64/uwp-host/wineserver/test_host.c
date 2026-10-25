#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

int uwph_server_start(const char *, int *);
int uwph_server_stop_and_join(void);
int uwph_server_join_failed(void);
static int run(char *const argv[])
{
    int status;
    pid_t pid = fork();
    if (!pid) { execvp(argv[0], argv); perror(argv[0]); _exit(127); }
    if (pid < 0 || waitpid(pid, &status, 0) < 0) return 1;
    return !WIFEXITED(status) || WEXITSTATUS(status);
}
/* Check /proc continuously while the client runs, without starting a server. */
static int no_server_process(void)
{
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    int ok = 1;
    if (!dir) return 0;
    while ((entry = readdir(dir)))
    {
        char path[PATH_MAX], cmd[4096];
        FILE *fp;
        size_t n;
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        if (!(fp = fopen(path, "r"))) continue;
        n = fread(cmd, 1, sizeof(cmd)-1, fp);
        fclose(fp);
        cmd[n] = 0;
        for (size_t i=0; i<n; ++i) if (!cmd[i]) cmd[i] = ' ';
        if (strstr(cmd, "wineserver64")) ok = 0;
    }
    closedir(dir);
    return ok;
}
int main(int argc, char **argv)
{
    char tmp[] = "/tmp/uwph-o6-XXXXXX", prefix[PATH_MAX], source[PATH_MAX], byte;
    char *copy[] = { "cp", "-a", source, prefix, NULL };
    char *writable[] = { "chmod", "-R", "u+w", prefix, NULL };
    char *remove[] = { "rm", "-rf", tmp, NULL };
    int fd, rc, status = 0, good = 1;
    pid_t client;
    struct pollfd ready;
    if (argc != 4 || strcmp(argv[1],"--prefix") || strcmp(argv[3],"--selftest")) return 2;
    if (!realpath(argv[2],source) || !mkdtemp(tmp)) { perror("prefix"); return 1; }
    snprintf(prefix,sizeof(prefix),"%s/prefix",tmp);
    if (run(copy) || run(writable)) return 1;
    /* All host/client paths are absolute before Wine changes process cwd. */
    setenv("WINESERVER","/bin/false",1);
    setenv("WINEDEBUG","-all",1);
    if ((rc = uwph_server_start(prefix,&fd))) { fprintf(stderr,"start rc=%d\n",rc); return 1; }
    ready = (struct pollfd){ fd, POLLIN, 0 };
    if (poll(&ready,1,10000) <= 0 || read(fd,&byte,1) != 1 || byte != 'R')
    { fprintf(stderr,"server startup failed\n"); close(fd); uwph_server_join_failed(); return 1; }
    close(fd);
    puts("PASS hosted server ready (socket listening)"); fflush(stdout);
    good = no_server_process();
    client = fork();
    if (!client)
    {
        char *args[] = { "wine", "cmd", "/c", "echo", "R4W-O6-OK", NULL };
        execvp(args[0],args); perror("wine"); _exit(127);
    }
    if (client < 0) good = 0;
    else
    {
        unsigned ticks = 0;
        while (waitpid(client,&status,WNOHANG) == 0)
        {
            good &= no_server_process();
            if (++ticks == 600) { kill(client,SIGKILL); waitpid(client,&status,0); good = 0; break; }
            usleep(100000);
        }
        good &= WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    rc = uwph_server_stop_and_join();
    printf("server thread joined rc=%d\n",rc);
    if (good) puts("PASS no wineserver64 process during run");
    else puts("FAIL client status or wineserver64 absence check (including pre-existing processes)");
    run(remove);
    return rc || !good;
}
