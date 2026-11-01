/*
 * x64native: run a static x86-64 Linux ELF natively in-process, with every
 * guest syscall intercepted (approach 2: binary passthrough + syscall trapping).
 *
 * Mechanism: Linux Syscall User Dispatch (PR_SET_SYSCALL_USER_DISPATCH, 5.11+).
 * All syscalls executed outside a small "allowed" trampoline page raise SIGSYS
 * while the selector byte is BLOCK. The SIGSYS handler emulates the syscall
 * (here: forwards/filters it to the host, logging each one) and resumes the guest.
 * The guest's FS base (TLS) is swapped with the host's on handler entry/exit.
 *
 * Build: gcc -O2 -fno-stack-protector -o x64native x64native.c
 * Usage: x64native [-v] guest-elf [args...]
 */
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <asm/prctl.h>

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#define PR_SYS_DISPATCH_OFF 0
#define PR_SYS_DISPATCH_ON 1
#define SYSCALL_DISPATCH_FILTER_ALLOW 0
#define SYSCALL_DISPATCH_FILTER_BLOCK 1
#endif

static volatile char selector = SYSCALL_DISPATCH_FILTER_ALLOW;
static uint64_t host_fs, guest_fs;
static int verbose;
static unsigned long syscallCount;
/* Guest brk heap: kept separate from the host's brk so guest and host malloc never collide. */
static uint64_t brkStart, brkCur, brkMappedEnd;

/* Trampoline page: the only place syscalls bypass dispatch. Layout:
 *   +0  raw_syscall(nr, a1..a6) -> rax      (System V args -> syscall regs)
 *   +64 rt_sigreturn restorer
 */
static uint8_t *tramp;
typedef long (*raw_syscall_fn)(long, long, long, long, long, long, long);
#define RAW ((raw_syscall_fn)tramp)

static void build_trampoline(void) {
    static const uint8_t raw[] = {
        0x48, 0x89, 0xf8,             /* mov rax, rdi */
        0x48, 0x89, 0xf7,             /* mov rdi, rsi */
        0x48, 0x89, 0xd6,             /* mov rsi, rdx */
        0x48, 0x89, 0xca,             /* mov rdx, rcx */
        0x4d, 0x89, 0xc2,             /* mov r10, r8  */
        0x4d, 0x89, 0xc8,             /* mov r8, r9   */
        0x4c, 0x8b, 0x4c, 0x24, 0x08, /* mov r9, [rsp+8] */
        0x0f, 0x05,                   /* syscall */
        0xc3                          /* ret */
    };
    static const uint8_t restorer[] = {
        0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00, /* mov rax, 15 (rt_sigreturn) */
        0x0f, 0x05                                /* syscall */
    };
    tramp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(tramp, raw, sizeof(raw));
    memcpy(tramp + 64, restorer, sizeof(restorer));
    mprotect(tramp, 4096, PROT_READ | PROT_EXEC);
}

static void die(const char *msg) { perror(msg); exit(1); }

/* Emulate one guest syscall. Returns the value to place in RAX. */
static long guest_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    switch (nr) {
    case SYS_arch_prctl:
        if (a1 == ARCH_SET_FS) { guest_fs = (uint64_t)a2; return 0; }
        if (a1 == ARCH_GET_FS) { *(uint64_t *)a2 = guest_fs; return 0; }
        break;
    case SYS_brk: {
        uint64_t want = (uint64_t)a1;
        if (want < brkStart) return (long)brkCur;
        uint64_t need = (want + 0xfff) & ~0xfffULL;
        if (need > brkMappedEnd) {
            if (RAW(SYS_mmap, (long)brkMappedEnd, (long)(need - brkMappedEnd), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (long)brkMappedEnd)
                return (long)brkCur;
            brkMappedEnd = need;
        }
        brkCur = want;
        return (long)brkCur;
    }
    case SYS_set_robust_list:
        return 0; /* don't replace the host thread's robust list */
    case SYS_rt_sigaction:
    case SYS_rt_sigprocmask:
        /* Guest signal handling would need full emulation (SIGSYS must stay ours). */
        if (nr == SYS_rt_sigaction && a1 == SIGSYS) return -22;
        break;
    }
    return RAW(nr, a1, a2, a3, a4, a5, a6);
}

static void sigsys_handler(int sig, siginfo_t *info, void *vctx) {
    (void)sig; (void)info;
    selector = SYSCALL_DISPATCH_FILTER_ALLOW;
    /* Switch FS to the host's TLS before touching any libc state. */
    RAW(SYS_arch_prctl, ARCH_GET_FS, (long)&guest_fs, 0, 0, 0, 0);
    RAW(SYS_arch_prctl, ARCH_SET_FS, (long)host_fs, 0, 0, 0, 0);

    ucontext_t *uc = vctx;
    greg_t *g = uc->uc_mcontext.gregs;
    long nr = g[REG_RAX];
    syscallCount++;
    long ret = guest_syscall(nr, g[REG_RDI], g[REG_RSI], g[REG_RDX], g[REG_R10], g[REG_R8], g[REG_R9]);
    if (verbose)
        fprintf(stderr, "[x64native] syscall %ld(%lx, %lx, %lx) = %ld  (rip=%llx)\n", nr,
                (long)g[REG_RDI], (long)g[REG_RSI], (long)g[REG_RDX], ret, (unsigned long long)g[REG_RIP]);
    if (nr == SYS_exit_group || nr == SYS_exit) _exit((int)g[REG_RDI]);
    g[REG_RAX] = ret;

    RAW(SYS_arch_prctl, ARCH_SET_FS, (long)guest_fs, 0, 0, 0, 0);
    selector = SYSCALL_DISPATCH_FILTER_BLOCK;
    /* returns through the restorer inside the allowed trampoline page */
}

struct kernel_sigaction { void *handler; unsigned long flags; void *restorer; uint64_t mask; };

static void start_guest(uint64_t entry, uint64_t sp) __attribute__((noreturn));
static void start_guest(uint64_t entry, uint64_t sp) {
    selector = SYSCALL_DISPATCH_FILTER_BLOCK;
    __asm__ volatile(
        "mov %0, %%rsp\n"
        "xor %%edx, %%edx\n" /* rdx = 0: no rtld fini */
        "xor %%ebp, %%ebp\n"
        "jmp *%1\n" ::"r"(sp), "r"(entry) : "memory");
    __builtin_unreachable();
}

int main(int argc, char **argv, char **envp) {
    int argi = 1;
    if (argi < argc && !strcmp(argv[argi], "-v")) { verbose = 1; argi++; }
    if (argi >= argc) { fprintf(stderr, "usage: %s [-v] guest-elf [args]\n", argv[0]); return 2; }

    int fd = open(argv[argi], O_RDONLY);
    if (fd < 0) die("open");
    struct stat st; fstat(fd, &st);
    uint8_t *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_X86_64) {
        fprintf(stderr, "not an x86-64 ELF\n"); return 1;
    }
    for (int i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_INTERP) { fprintf(stderr, "dynamic ELF not supported yet\n"); return 1; }
    }
    uint64_t bias = eh->e_type == ET_DYN ? 0x555555000000ULL : 0;
    uint64_t phdrAddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint64_t start = (ph->p_vaddr + bias) & ~0xfffULL;
        uint64_t end = (ph->p_vaddr + bias + ph->p_memsz + 0xfff) & ~0xfffULL;
        if (mmap((void *)start, end - start, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)start)
            die("mmap segment");
        memcpy((void *)(ph->p_vaddr + bias), file + ph->p_offset, ph->p_filesz);
        int prot = (ph->p_flags & PF_R ? PROT_READ : 0) | (ph->p_flags & PF_W ? PROT_WRITE : 0) | (ph->p_flags & PF_X ? PROT_EXEC : 0);
        mprotect((void *)start, end - start, prot);
        if (end > brkStart) brkStart = end;
        if (ph->p_offset <= eh->e_phoff && eh->e_phoff < ph->p_offset + ph->p_filesz)
            phdrAddr = ph->p_vaddr + bias + (eh->e_phoff - ph->p_offset);
    }

    brkCur = brkMappedEnd = brkStart;
    /* Guest stack with argv/envp/auxv, as the Linux kernel builds it. */
    size_t stackSize = 8 << 20;
    uint8_t *stack = mmap(NULL, stackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
    uint8_t *top = stack + stackSize;
    int gargc = argc - argi, envc = 0;
    while (envp[envc]) envc++;
    char **gargv = alloca(sizeof(char *) * gargc), **genv = alloca(sizeof(char *) * (envc + 1));
    for (int i = 0; i < gargc; i++) { size_t l = strlen(argv[argi + i]) + 1; top -= l; memcpy(top, argv[argi + i], l); gargv[i] = (char *)top; }
    for (int i = 0; i < envc; i++) { size_t l = strlen(envp[i]) + 1; top -= l; memcpy(top, envp[i], l); genv[i] = (char *)top; }
    top -= 16; uint8_t *random = top; for (int i = 0; i < 16; i++) random[i] = (uint8_t)(rand());
    top -= 16; memcpy(top, "x86_64", 7); char *platform = (char *)top;
    uint64_t auxv[][2] = {
        {AT_PHDR, phdrAddr}, {AT_PHENT, eh->e_phentsize}, {AT_PHNUM, eh->e_phnum},
        {AT_PAGESZ, 4096}, {AT_BASE, 0}, {AT_FLAGS, 0}, {AT_ENTRY, eh->e_entry + bias},
        {AT_UID, getuid()}, {AT_EUID, geteuid()}, {AT_GID, getgid()}, {AT_EGID, getegid()},
        {AT_SECURE, 0}, {AT_RANDOM, (uint64_t)random}, {AT_HWCAP, getauxval(AT_HWCAP)},
        {AT_HWCAP2, getauxval(AT_HWCAP2)}, {AT_CLKTCK, 100}, {AT_PLATFORM, (uint64_t)platform},
        {AT_EXECFN, (uint64_t)gargv[0]}, {AT_NULL, 0}};
    size_t words = 1 + gargc + 1 + envc + 1 + sizeof(auxv) / 8;
    uint64_t *sp = (uint64_t *)(((uintptr_t)top - words * 8) & ~0xfULL);
    uint64_t *p = sp;
    *p++ = gargc;
    for (int i = 0; i < gargc; i++) *p++ = (uint64_t)gargv[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++) *p++ = (uint64_t)genv[i];
    *p++ = 0;
    memcpy(p, auxv, sizeof(auxv));

    build_trampoline();
    syscall(SYS_arch_prctl, ARCH_GET_FS, &host_fs);
    struct kernel_sigaction ksa = {sigsys_handler, SA_SIGINFO | 0x04000000 /* SA_RESTORER */ | SA_NODEFER, tramp + 64, 0};
    if (syscall(SYS_rt_sigaction, SIGSYS, &ksa, NULL, 8)) die("rt_sigaction");
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, (unsigned long)tramp, 4096, &selector)) die("prctl SUD");
    if (verbose) fprintf(stderr, "[x64native] entry=%llx sp=%p\n", (unsigned long long)(eh->e_entry + bias), (void *)sp);
    start_guest(eh->e_entry + bias, (uint64_t)sp);
}
