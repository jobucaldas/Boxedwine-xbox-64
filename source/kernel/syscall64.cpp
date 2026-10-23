/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#include "../emulation/cpu/x64/x64CPU.h"
#include "ksignal.h"

// x86-64 Linux syscall ABI on top of the (i386) BoxedWine kernel.
//
// Most x86-64 syscalls have an i386 twin with an identical argument list and
// identical struct layouts; those are forwarded to the existing i386 handler
// table by staging the arguments in the i386 registers. Syscalls whose structs
// contain longs/pointers (stat, iovec, sigaction, timeval, ...) are converted
// here. All guest memory is below 4GB, so pointers fit the 32-bit kernel APIs.

U32 ksyscallI386(CPU* cpu, U32 nr);

static U32 syscallLog64 = 0;

static void initLog() {
    static bool init = false;
    if (!init) {
        init = true;
        const char* e = getenv("BOXEDWINE_X64_STRACE");
        syscallLog64 = (e && e[0] == '1') ? 1 : 0;
    }
}

static U64 bridge(X64CPU* cpu, U32 nr32, U64 a1 = 0, U64 a2 = 0, U64 a3 = 0, U64 a4 = 0, U64 a5 = 0, U64 a6 = 0) {
    cpu->reg[3].u32 = (U32)a1; // EBX
    cpu->reg[1].u32 = (U32)a2; // ECX
    cpu->reg[2].u32 = (U32)a3; // EDX
    cpu->reg[6].u32 = (U32)a4; // ESI
    cpu->reg[7].u32 = (U32)a5; // EDI
    cpu->reg[5].u32 = (U32)a6; // EBP
    cpu->reg[0].u32 = nr32;
    cpu->reg[4].u32 = (U32)cpu->r[4];
    return ksyscallI386(cpu, nr32);
}

// Per-thread scratch page in guest memory, used to stage i386 structs that
// are then converted to their x86-64 layout.
static U32 scratch(X64CPU* cpu) {
    static thread_local KThread* owner = nullptr;
    static thread_local U32 page = 0;
    if (owner != cpu->thread || !page) {
        page = cpu->memory->mmap(cpu->thread, 0, 8192, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
        owner = cpu->thread;
    }
    return page;
}

static bool isError(U32 result) {
    return result >= 0xFFFFF001;
}

// struct stat64 (i386, 96 bytes) -> struct stat (x86-64, 144 bytes)
static void convertStat(KMemory* m, U32 src, U64 dst) {
    U8 s[96];
    m->memcpy(s, src, 96);
    U64 dev, rdev, blocks, ino;
    S64 size;
    U32 mode, nlink, uid, gid, blksize, at, atn, mt, mtn, ct, ctn;
    memcpy(&dev, s + 0, 8);
    memcpy(&mode, s + 16, 4);
    memcpy(&nlink, s + 20, 4);
    memcpy(&uid, s + 24, 4);
    memcpy(&gid, s + 28, 4);
    memcpy(&rdev, s + 32, 8);
    memcpy(&size, s + 44, 8);
    memcpy(&blksize, s + 52, 4);
    memcpy(&blocks, s + 56, 8);
    memcpy(&at, s + 64, 4);
    memcpy(&atn, s + 68, 4);
    memcpy(&mt, s + 72, 4);
    memcpy(&mtn, s + 76, 4);
    memcpy(&ct, s + 80, 4);
    memcpy(&ctn, s + 84, 4);
    memcpy(&ino, s + 88, 8);

    U8 d[144];
    memset(d, 0, sizeof(d));
    U64 v;
    memcpy(d + 0, &dev, 8);
    memcpy(d + 8, &ino, 8);
    v = nlink; memcpy(d + 16, &v, 8);
    memcpy(d + 24, &mode, 4);
    memcpy(d + 28, &uid, 4);
    memcpy(d + 32, &gid, 4);
    memcpy(d + 40, &rdev, 8);
    memcpy(d + 48, &size, 8);
    v = blksize; memcpy(d + 56, &v, 8);
    memcpy(d + 64, &blocks, 8);
    v = at; memcpy(d + 72, &v, 8);
    v = atn; memcpy(d + 80, &v, 8);
    v = mt; memcpy(d + 88, &v, 8);
    v = mtn; memcpy(d + 96, &v, 8);
    v = ct; memcpy(d + 104, &v, 8);
    v = ctn; memcpy(d + 112, &v, 8);
    m->memcpy((U32)dst, d, 144);
}

// struct iovec[] (16 bytes each) -> i386 iovec[] (8 bytes each)
static U32 convertIov(X64CPU* cpu, U64 iov, U64 count) {
    U32 s = scratch(cpu);
    if (count > 1024) {
        count = 1024;
    }
    for (U32 i = 0; i < count; i++) {
        U64 base = cpu->read64(iov + i * 16);
        U64 len = cpu->read64(iov + i * 16 + 8);
        cpu->memory->writed(s + i * 8, (U32)base);
        cpu->memory->writed(s + i * 8 + 4, (U32)len);
    }
    return s;
}

static U64 rtSigaction(X64CPU* cpu, U64 sig, U64 act, U64 oact) {
    if (sig == 0 || sig == K_SIGKILL || sig == K_SIGSTOP || sig >= MAX_SIG_ACTIONS) {
        return (U64)-K_EINVAL;
    }
    KSigAction& sa = cpu->thread->process->sigActions[sig];
    if (oact) {
        cpu->write64(oact, sa.handlerAndSigAction);
        cpu->write64(oact + 8, sa.flags);
        cpu->write64(oact + 16, sa.restorer);
        cpu->write64(oact + 24, sa.mask);
    }
    if (act) {
        sa.handlerAndSigAction = (U32)cpu->read64(act);
        sa.flags = (U32)cpu->read64(act + 8);
        sa.restorer = (U32)cpu->read64(act + 16);
        sa.mask = cpu->read64(act + 24);
    }
    return 0;
}

static U64 sigaltstack64(X64CPU* cpu, U64 ss, U64 oss) {
    if (oss) {
        cpu->write64(oss, cpu->sigAltStack);
        cpu->write32(oss + 8, cpu->sigAltStack ? 0 : 2 /* SS_DISABLE */);
        cpu->write32(oss + 12, 0);
        cpu->write64(oss + 16, cpu->sigAltStackSize);
    }
    if (ss) {
        U32 flags = cpu->read32(ss + 8);
        if (flags & 2) {
            cpu->sigAltStack = 0;
            cpu->sigAltStackSize = 0;
        } else {
            cpu->sigAltStack = cpu->read64(ss);
            cpu->sigAltStackSize = cpu->read64(ss + 16);
        }
    }
    return 0;
}

static U64 arch_prctl(X64CPU* cpu, U64 code, U64 addr) {
    switch (code) {
    case 0x1001: cpu->gsBase = addr; return 0;              // ARCH_SET_GS
    case 0x1002: cpu->fsBase = addr; return 0;              // ARCH_SET_FS
    case 0x1003: cpu->write64(addr, cpu->fsBase); return 0; // ARCH_GET_FS
    case 0x1004: cpu->write64(addr, cpu->gsBase); return 0; // ARCH_GET_GS
    case 0x3001: return (U64)-K_EINVAL;                     // ARCH_CET_STATUS
    default: return (U64)-K_EINVAL;
    }
}

static U64 x64Mmap(X64CPU* cpu, U64 addr, U64 len, U64 prot, U64 flags, U64 fd, U64 off) {
    if ((addr >> 32) || (len >> 32)) {
        if (flags & K_MAP_FIXED) {
            return (U64)-K_ENOMEM;
        }
        if (len >> 32) {
            return (U64)-K_ENOMEM;
        }
        addr = 0; // hint outside the guest window
    }
    flags &= ~0x40ULL; // MAP_32BIT: everything is below 4GB anyway
    return cpu->memory->mmap(cpu->thread, (U32)addr, (U32)len, (S32)prot, (S32)flags, (FD)fd, off);
}

static U64 x64Exit(X64CPU* cpu, U64 code, bool group) {
    U32 nr32 = group ? 252 : 1;
    return bridge(cpu, nr32, code);
}

static U64 doSyscall64(X64CPU* cpu, U64 nr, U64 a1, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6) {
    KMemory* memory = cpu->memory;
    switch (nr) {
    case 0: return bridge(cpu, 3, a1, a2, a3);          // read
    case 1: return bridge(cpu, 4, a1, a2, a3);          // write
    case 2: return bridge(cpu, 5, a1, a2, a3);          // open
    case 3: return bridge(cpu, 6, a1);                  // close
    case 4: case 5: case 6: {                           // stat, fstat, lstat
        U32 s = scratch(cpu);
        U32 result = (U32)bridge(cpu, nr == 4 ? 195 : (nr == 5 ? 197 : 196), a1, s);
        if (!isError(result)) convertStat(memory, s, a2);
        return result;
    }
    case 7: return bridge(cpu, 168, a1, a2, a3);        // poll
    case 8: {                                           // lseek
        U32 s = scratch(cpu);
        U32 result = (U32)bridge(cpu, 140, a1, (U32)(a2 >> 32), (U32)a2, s, a3);
        if (isError(result)) return result;
        return memory->readq(s);
    }
    case 9: return x64Mmap(cpu, a1, a2, a3, a4, a5, a6);
    case 10: return bridge(cpu, 125, a1, a2, a3);       // mprotect
    case 11: return bridge(cpu, 91, a1, a2);            // munmap
    case 12: return bridge(cpu, 45, a1);                // brk
    case 13: return rtSigaction(cpu, a1, a2, a3);       // rt_sigaction
    case 14: return cpu->thread->sigprocmask((U32)a1, (U32)a2, (U32)a3, (U32)a4);
    case 15: return x64Sigreturn(cpu);                  // rt_sigreturn
    case 16: return bridge(cpu, 54, a1, a2, a3);        // ioctl
    case 17: return cpu->thread->process->pread64(cpu->thread, (FD)a1, (U32)a2, (U32)a3, a4);
    case 18: return cpu->thread->process->pwrite64(cpu->thread, (FD)a1, (U32)a2, (U32)a3, a4);
    case 19: return bridge(cpu, 145, a1, convertIov(cpu, a2, a3), a3); // readv
    case 20: return bridge(cpu, 146, a1, convertIov(cpu, a2, a3), a3); // writev
    case 21: return bridge(cpu, 33, a1, a2);            // access
    case 22: return bridge(cpu, 42, a1);                // pipe
    case 24: return bridge(cpu, 158);                   // sched_yield
    case 25: return bridge(cpu, 163, a1, a2, a3, a4, a5); // mremap
    case 26: return bridge(cpu, 144, a1, a2, a3);       // msync
    case 27: return bridge(cpu, 218, a1, a2, a3);       // mincore
    case 28: return bridge(cpu, 219, a1, a2, a3);       // madvise
    case 32: return bridge(cpu, 41, a1);                // dup
    case 33: return bridge(cpu, 63, a1, a2);            // dup2
    case 35: return bridge(cpu, 407, 0, 0, a1, a2);     // nanosleep -> clock_nanosleep_time64
    case 39: return bridge(cpu, 20);                    // getpid
    case 60: return x64Exit(cpu, a1, false);            // exit
    case 61: {                                          // wait4
        if (a4) {
            for (int i = 0; i < 144; i += 8) cpu->write64(a4 + i, 0);
        }
        return bridge(cpu, 114, a1, a2, a3, 0);
    }
    case 62: return bridge(cpu, 37, a1, a2);            // kill
    case 63: {                                          // uname
        U32 result = (U32)bridge(cpu, 122, a1);
        if (!isError(result)) {
            memory->memset((U32)a1 + 65 * 4, 0, 65);
            memory->strcpy((U32)a1 + 65 * 4, "x86_64");
        }
        return result;
    }
    case 72: return bridge(cpu, 221, a1, a2, a3);       // fcntl -> fcntl64
    case 73: return bridge(cpu, 143, a1, a2);           // flock
    case 74: return bridge(cpu, 118, a1);               // fsync
    case 75: return bridge(cpu, 148, a1);               // fdatasync
    case 77: return bridge(cpu, 194, a1, (U32)a2, (U32)(a2 >> 32)); // ftruncate -> ftruncate64
    case 79: return bridge(cpu, 183, a1, a2);           // getcwd
    case 80: return bridge(cpu, 12, a1);                // chdir
    case 81: return bridge(cpu, 133, a1);               // fchdir
    case 82: return bridge(cpu, 38, a1, a2);            // rename
    case 83: return bridge(cpu, 39, a1, a2);            // mkdir
    case 84: return bridge(cpu, 40, a1);                // rmdir
    case 86: return bridge(cpu, 9, a1, a2);             // link
    case 87: return bridge(cpu, 10, a1);                // unlink
    case 88: return bridge(cpu, 83, a1, a2);            // symlink
    case 89: return bridge(cpu, 85, a1, a2, a3);        // readlink
    case 90: return bridge(cpu, 15, a1, a2);            // chmod
    case 91: return bridge(cpu, 94, a1, a2);            // fchmod
    case 92: return bridge(cpu, 212, a1, a2, a3);       // chown
    case 93: return bridge(cpu, 207, a1, a2, a3);       // fchown
    case 94: return bridge(cpu, 198, a1, a2, a3);       // lchown
    case 95: return bridge(cpu, 60, a1);                // umask
    case 96: {                                          // gettimeofday
        if (a1) {
            U64 t = KSystem::getSystemTimeAsMicroSeconds();
            cpu->write64(a1, t / 1000000);
            cpu->write64(a1 + 8, t % 1000000);
        }
        if (a2) {
            cpu->write64(a2, 0);
        }
        return 0;
    }
    case 97: return bridge(cpu, 340, 0, a1, 0, a2);     // getrlimit -> prlimit64
    case 98: {                                          // getrusage
        for (int i = 0; i < 144; i += 8) cpu->write64(a2 + i, 0);
        return 0;
    }
    case 99: {                                          // sysinfo
        U32 s = scratch(cpu);
        U32 result = (U32)bridge(cpu, 116, s);
        if (isError(result)) return result;
        // i386: long uptime, loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap (u32), u16 procs, pad, totalhigh, freehigh, mem_unit
        for (int i = 0; i < 112; i += 8) cpu->write64(a1 + i, 0);
        for (int i = 0; i < 10; i++) cpu->write64(a1 + i * 8, memory->readd(s + i * 4) & 0xFFFFFFFFULL);
        cpu->write16(a1 + 80, memory->readw(s + 40));
        cpu->write32(a1 + 104, memory->readd(s + 52)); // mem_unit
        return 0;
    }
    case 100: {                                         // times
        if (a1) {
            for (int i = 0; i < 32; i += 8) cpu->write64(a1 + i, 0);
        }
        return KSystem::getMicroCounter() / 10000;
    }
    case 102: return bridge(cpu, 199);                  // getuid
    case 104: return bridge(cpu, 200);                  // getgid
    case 105: return bridge(cpu, 213, a1);              // setuid
    case 106: return bridge(cpu, 214, a1);              // setgid
    case 107: return bridge(cpu, 201);                  // geteuid
    case 108: return bridge(cpu, 202);                  // getegid
    case 109: return bridge(cpu, 57, a1, a2);           // setpgid
    case 110: return bridge(cpu, 64);                   // getppid
    case 111: return bridge(cpu, 65);                   // getpgrp
    case 112: return bridge(cpu, 66);                   // setsid
    case 115: return bridge(cpu, 205, a1, a2);          // getgroups
    case 116: return bridge(cpu, 206, a1, a2);          // setgroups
    case 117: return bridge(cpu, 208, a1, a2, a3);      // setresuid
    case 118: return bridge(cpu, 209, a1, a2, a3);      // getresuid
    case 119: return bridge(cpu, 210, a1, a2, a3);      // setresgid
    case 120: return bridge(cpu, 211, a1, a2, a3);      // getresgid
    case 121: return bridge(cpu, 132, a1);              // getpgid
    case 131: return sigaltstack64(cpu, a1, a2);        // sigaltstack
    case 157: return bridge(cpu, 172, a1, a2, a3, a4, a5); // prctl
    case 158: return arch_prctl(cpu, a1, a2);           // arch_prctl
    case 160: return bridge(cpu, 340, 0, a1, a2, 0);    // setrlimit -> prlimit64
    case 186: return bridge(cpu, 224);                  // gettid
    case 200: return bridge(cpu, 270, bridge(cpu, 20), a1, a2); // tkill -> tgkill
    case 201: {                                         // time
        U64 t = KSystem::getSystemTimeAsMicroSeconds() / 1000000;
        if (a1) cpu->write64(a1, t);
        return t;
    }
    case 202: return bridge(cpu, 422, a1, a2, a3, a4, a5, a6); // futex -> futex_time64
    case 203: return bridge(cpu, 241, a1, a2, a3);      // sched_setaffinity
    case 204: return bridge(cpu, 242, a1, a2, a3);      // sched_getaffinity
    case 213: return bridge(cpu, 254, a1);              // epoll_create
    case 217: return bridge(cpu, 220, a1, a2, a3);      // getdents64
    case 218: return bridge(cpu, 258, a1);              // set_tid_address
    case 228: return bridge(cpu, 403, a1, a2);          // clock_gettime -> clock_gettime64
    case 229: return bridge(cpu, 406, a1, a2);          // clock_getres -> clock_getres_time64
    case 230: return bridge(cpu, 407, a1, a2, a3, a4);  // clock_nanosleep -> time64
    case 231: return x64Exit(cpu, a1, true);            // exit_group
    case 232: return bridge(cpu, 256, a1, a2, a3, a4);  // epoll_wait
    case 233: return bridge(cpu, 255, a1, a2, a3, a4);  // epoll_ctl
    case 234: return bridge(cpu, 270, a1, a2, a3);      // tgkill
    case 257: return bridge(cpu, 295, a1, a2, a3, a4);  // openat
    case 258: return bridge(cpu, 296, a1, a2, a3);      // mkdirat
    case 260: return bridge(cpu, 298, a1, a2, a3, a4, a5); // fchownat
    case 262: {                                         // newfstatat
        U32 s = scratch(cpu);
        U32 result = (U32)bridge(cpu, 300, a1, a2, s, a4);
        if (!isError(result)) convertStat(memory, s, a3);
        return result;
    }
    case 263: return bridge(cpu, 301, a1, a2, a3);      // unlinkat
    case 264: return bridge(cpu, 302, a1, a2, a3, a4);  // renameat
    case 266: return bridge(cpu, 304, a1, a2, a3);      // symlinkat
    case 267: return bridge(cpu, 305, a1, a2, a3, a4);  // readlinkat
    case 268: return bridge(cpu, 306, a1, a2, a3);      // fchmodat
    case 269: return bridge(cpu, 307, a1, a2, a3);      // faccessat
    case 273: return bridge(cpu, 311, a1, a2);          // set_robust_list
    case 280: return bridge(cpu, 412, a1, a2, a3, a4);  // utimensat -> time64
    case 284: return bridge(cpu, 328, a1, 0);           // eventfd
    case 290: return bridge(cpu, 328, a1, a2);          // eventfd2
    case 291: return bridge(cpu, 329, a1);              // epoll_create1
    case 292: return bridge(cpu, 330, a1, a2, a3);      // dup3
    case 293: return bridge(cpu, 331, a1, a2);          // pipe2
    case 302: return bridge(cpu, 340, a1, a2, a3, a4);  // prlimit64
    case 316: return bridge(cpu, 353, a1, a2, a3, a4, a5); // renameat2
    case 318: return bridge(cpu, 355, a1, a2, a3);      // getrandom
    case 319: return bridge(cpu, 356, a1, a2);          // memfd_create
    case 332: return bridge(cpu, 383, a1, a2, a3, a4, a5); // statx
    case 334: return (U64)-K_ENOSYS;                    // rseq
    case 439: return bridge(cpu, 439, a1, a2, a3, a4);  // faccessat2
    default:
        break;
    }
    klog_fmt("x64: unimplemented syscall %d (%llx %llx %llx)", (int)nr, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3);
    return (U64)-K_ENOSYS;
}

void ksyscall64(X64CPU* cpu) {
    initLog();
    U64 nr = cpu->r[0];
    U64 startRip = cpu->rip;
    if (cpu->thread->terminating) {
        terminateCurrentThread(cpu->thread);
        cpu->syscallInvalidatedFetch = true;
        return;
    }
    U64 result = doSyscall64(cpu, nr, cpu->r[7], cpu->r[6], cpu->r[2], cpu->r[10], cpu->r[8], cpu->r[9]);
    cpu->syscallInvalidatedFetch = true;
    if (nr == 15) {
        // rt_sigreturn restored the whole register file
        return;
    }
    U32 r32 = (U32)result;
    if (r32 == (U32)-K_WAIT) {
        cpu->rip = startRip - 2; // restart the syscall instruction
        return;
    }
    if (r32 == (U32)-K_CONTINUE) {
        return;
    }
    if ((result >> 32) == 0 && isError(r32)) {
        result = (U64)(S64)(S32)r32;
    }
    if (syscallLog64) {
        klog_fmt("x64 syscall %d(%llx, %llx, %llx, %llx) = %llx", (int)nr, (unsigned long long)cpu->r[7], (unsigned long long)cpu->r[6], (unsigned long long)cpu->r[2], (unsigned long long)cpu->r[10], (unsigned long long)result);
    }
    cpu->r[0] = result;
}
