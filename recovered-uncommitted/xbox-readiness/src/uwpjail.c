/* uwpjail: approximates Xbox UWP (Dev Mode) process constraints on Linux for testing.
 * Applied to the calling process, then exec()s the command (filters are inherited).
 *   --wx            seccomp: mmap/mprotect/pkey_mprotect with PROT_WRITE|PROT_EXEC -> EACCES
 *                   (UWP: VirtualProtectFromApp rejects PAGE_EXECUTE_READWRITE; RX only via codeGeneration)
 *   --nofork        seccomp: fork/vfork/clone-without-CLONE_THREAD -> EPERM, clone3 -> ENOSYS
 *                   (UWP: an app cannot spawn arbitrary child processes; threads are fine)
 *   --noexec        seccomp: execve/execveat -> EPERM (applied last; cmd itself is exec'd first via --noexec-after-start is not possible, so
 *                   this is only for probes that should never exec)
 *   --net-uwp       landlock net (ABI>=4): TCP bind only to ports 0 and >=1024; TCP connect to any port
 *   --net-xbox      like --net-uwp, but TCP bind to 57344-65535 is denied
 *   --ro PATH / --rw PATH   landlock fs: only these trees readable / writable (everything else denied)
 * Reports what it applied on stderr. Exit 125 on setup failure. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (1ULL << 1)
#endif

static int ll_create(const struct landlock_ruleset_attr *a, size_t s, unsigned f) { return syscall(SYS_landlock_create_ruleset, a, s, f); }
static int ll_add(int fd, int t, const void *a, unsigned f) { return syscall(SYS_landlock_add_rule, fd, t, a, f); }
static int ll_restrict(int fd) { return syscall(SYS_landlock_restrict_self, fd, 0); }

#define ARG_LO(n) (offsetof(struct seccomp_data, args[n]))
#define NR_OFF (offsetof(struct seccomp_data, nr))

int main(int argc, char **argv)
{
    int wx = 0, nofork = 0, noexec = 0, netuwp = 0, netxbox = 0, i;
    const char *ro[64], *rw[64]; int nro = 0, nrw = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "--wx")) wx = 1;
        else if (!strcmp(argv[i], "--nofork")) nofork = 1;
        else if (!strcmp(argv[i], "--noexec")) noexec = 1;
        else if (!strcmp(argv[i], "--net-uwp")) netuwp = 1;
        else if (!strcmp(argv[i], "--net-xbox")) { netuwp = 1; netxbox = 1; }
        else if (!strcmp(argv[i], "--ro") && i + 1 < argc) ro[nro++] = argv[++i];
        else if (!strcmp(argv[i], "--rw") && i + 1 < argc) rw[nrw++] = argv[++i];
        else { fprintf(stderr, "uwpjail: bad arg %s\n", argv[i]); return 125; }
    }
    if (i >= argc) { fprintf(stderr, "usage: uwpjail [opts] -- cmd...\n"); return 125; }

    if (netuwp || nro || nrw) {
        int abi = ll_create(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
        if (abi < 1) { perror("uwpjail: landlock unavailable"); return 125; }
        struct landlock_ruleset_attr attr = {0};
        __u64 fs_all = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
            LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
            LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
            LANDLOCK_ACCESS_FS_MAKE_SYM;
        if (abi >= 2) fs_all |= LANDLOCK_ACCESS_FS_REFER;
        if (abi >= 3) fs_all |= LANDLOCK_ACCESS_FS_TRUNCATE;
        if (nro || nrw) attr.handled_access_fs = fs_all;
        if (netuwp) {
            if (abi < 4) { fprintf(stderr, "uwpjail: landlock ABI %d has no net support\n", abi); return 125; }
            attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
        }
        int fd = ll_create(&attr, sizeof attr, 0);
        if (fd < 0) { perror("uwpjail: landlock_create_ruleset"); return 125; }
        __u64 ro_acc = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
        for (int k = 0; k < nro + nrw; k++) {
            const char *p = k < nro ? ro[k] : rw[k - nro];
            struct landlock_path_beneath_attr pb = { .allowed_access = k < nro ? ro_acc : fs_all };
            pb.parent_fd = open(p, O_PATH | O_CLOEXEC);
            if (pb.parent_fd < 0) { fprintf(stderr, "uwpjail: open %s: %s\n", p, strerror(errno)); return 125; }
            if (ll_add(fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0)) { fprintf(stderr, "uwpjail: rule %s: %s\n", p, strerror(errno)); return 125; }
            close(pb.parent_fd);
        }
        if (netuwp) {
            for (unsigned port = 0; port < 65536; port++) {
                struct landlock_net_port_attr np = { .port = port, .allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP };
                if (port == 0 || (port >= 1024 && (!netxbox || port < 57344))) np.allowed_access |= LANDLOCK_ACCESS_NET_BIND_TCP;
                if (ll_add(fd, LANDLOCK_RULE_NET_PORT, &np, 0)) { perror("uwpjail: net rule"); return 125; }
            }
        }
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || ll_restrict(fd)) { perror("uwpjail: landlock_restrict_self"); return 125; }
        fprintf(stderr, "uwpjail: landlock abi=%d fs=%s net=%s\n", abi, (nro || nrw) ? "on" : "off", netxbox ? "bind=0,1024-57343" : netuwp ? "bind>=1024" : "off");
    }

    if (wx || nofork || noexec) {
        struct sock_filter f[64]; int n = 0;
#define S(x) f[n++] = (struct sock_filter)x
        S(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
        S(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
        S(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
        S(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF));
        if (wx) {
            int nrs[3] = { __NR_mmap, __NR_mprotect, __NR_pkey_mprotect };
            for (int k = 0; k < 3; k++) {
                /* if nr == X: A = prot; if (A & 6) == 6 -> EACCES else reload nr and continue */
                S(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nrs[k], 0, 5));
                S(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(2)));
                S(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_WRITE | PROT_EXEC));
                S(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PROT_WRITE | PROT_EXEC, 0, 1));
                S(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES));
                S(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF));
            }
        }
        if (nofork) {
            S(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fork, 0, 1));
            S(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
            S(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_vfork, 0, 1));
            S(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
            S(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1));
            S(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS));
            S(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 4));
            S(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(0)));
            S(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0));
            S(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
            S(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF));
        }
        S(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        struct sock_fprog prog = { .len = n, .filter = f };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) { perror("uwpjail: seccomp"); return 125; }
        fprintf(stderr, "uwpjail: seccomp wx=%d nofork=%d (%d insns)\n", wx, nofork, n);
    }
    if (noexec) fprintf(stderr, "uwpjail: --noexec not implemented as filter (cmd must exec); ignored\n");
    execvp(argv[i], &argv[i]);
    fprintf(stderr, "uwpjail: exec %s: %s\n", argv[i], strerror(errno));
    return 127;
}
