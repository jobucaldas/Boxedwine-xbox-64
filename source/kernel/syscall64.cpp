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

// same values as the K_CLONE_* flags private to kprocess.cpp
#define K_CLONE_VM                0x00000100
#define K_CLONE_VFORK             0x00004000
#define K_CLONE_THREAD            0x00010000
#define K_CLONE_SETTLS            0x00080000
#define K_CLONE_PARENT_SETTID     0x00100000
#define K_CLONE_CHILD_CLEARTID    0x00200000
#define K_CLONE_CHILD_SETTID      0x01000000

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
    if (!cpu->scratchPage) {
        cpu->scratchPage = cpu->memory->mmap(cpu->thread, 0, 8192, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    }
    return cpu->scratchPage;
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

// struct statfs64 (i386, 84 bytes) -> struct statfs (x86-64, 120 bytes)
static void convertStatfs(X64CPU* cpu, U32 src, U64 dst) {
    KMemory* m = cpu->memory;
    cpu->write64(dst + 0, m->readd(src + 0));        // f_type
    cpu->write64(dst + 8, m->readd(src + 4));        // f_bsize
    for (int i = 0; i < 5; i++) {                    // f_blocks .. f_ffree
        cpu->write64(dst + 16 + i * 8, m->readq(src + 8 + i * 8));
    }
    cpu->write64(dst + 56, m->readq(src + 48));      // f_fsid
    cpu->write64(dst + 64, m->readd(src + 56));      // f_namelen
    cpu->write64(dst + 72, m->readd(src + 60));      // f_frsize
    cpu->write64(dst + 80, m->readd(src + 64));      // f_flags
    for (int i = 0; i < 4; i++) {
        cpu->write64(dst + 88 + i * 8, 0);
    }
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


// struct msghdr (x86-64, 56 bytes) <-> i386 msghdr (28 bytes) staged in the
// scratch page: [0] msghdr, [64] iovec[], [2048] control buffer.
#define SCRATCH_MSGHDR 0
#define SCRATCH_IOV 64
#define SCRATCH_CONTROL 2048
#define SCRATCH_CONTROL_SIZE 4096

static U32 convertMsgToI386(X64CPU* cpu, U64 msg, bool forRecv) {
    U32 s = scratch(cpu);
    KMemory* m = cpu->memory;
    U64 name = cpu->read64(msg);
    U32 namelen = cpu->read32(msg + 8);
    U64 iov = cpu->read64(msg + 16);
    U64 iovlen = cpu->read64(msg + 24);
    U64 control = cpu->read64(msg + 32);
    U64 controllen = cpu->read64(msg + 40);
    if (iovlen > 200) {
        iovlen = 200;
    }
    for (U32 i = 0; i < iovlen; i++) {
        m->writed(s + SCRATCH_IOV + i * 8, (U32)cpu->read64(iov + i * 16));
        m->writed(s + SCRATCH_IOV + i * 8 + 4, (U32)cpu->read64(iov + i * 16 + 8));
    }
    U32 control32 = 0;
    U32 controllen32 = 0;
    if (control && controllen) {
        control32 = s + SCRATCH_CONTROL;
        if (forRecv) {
            controllen32 = SCRATCH_CONTROL_SIZE;
        } else {
            // convert each cmsghdr: {u64 len; int level; int type; data} -> {u32 len; int level; int type; data}
            U64 pos = 0;
            while (pos + 16 <= controllen) {
                U64 len = cpu->read64(control + pos);
                if (len < 16 || pos + len > controllen) {
                    break;
                }
                U32 dataLen = (U32)(len - 16);
                m->writed(control32 + controllen32, 12 + dataLen);
                m->writed(control32 + controllen32 + 4, cpu->read32(control + pos + 8));
                m->writed(control32 + controllen32 + 8, cpu->read32(control + pos + 12));
                for (U32 i = 0; i < dataLen; i++) {
                    m->writeb(control32 + controllen32 + 12 + i, cpu->read8(control + pos + 16 + i));
                }
                controllen32 += (12 + dataLen + 3) & ~3;
                pos += (len + 7) & ~7ULL;
            }
        }
    }
    m->writed(s + 0, (U32)name);
    m->writed(s + 4, namelen);
    m->writed(s + 8, s + SCRATCH_IOV);
    m->writed(s + 12, (U32)iovlen);
    m->writed(s + 16, control32);
    m->writed(s + 20, controllen32);
    m->writed(s + 24, cpu->read32(msg + 48));
    return s;
}

static void convertRecvMsgFromI386(X64CPU* cpu, U32 s, U64 msg) {
    KMemory* m = cpu->memory;
    cpu->write32(msg + 8, m->readd(s + 4)); // namelen
    cpu->write32(msg + 48, m->readd(s + 24)); // flags
    U64 control = cpu->read64(msg + 32);
    U64 controllen = cpu->read64(msg + 40);
    U32 control32 = m->readd(s + 16);
    U32 controllen32 = m->readd(s + 20);
    U64 out = 0;
    if (control && control32) {
        U32 pos = 0;
        while (pos + 12 <= controllen32) {
            U32 len = m->readd(control32 + pos);
            if (len < 12 || pos + len > controllen32) {
                break;
            }
            U32 dataLen = len - 12;
            if (out + 16 + dataLen > controllen) {
                cpu->write32(msg + 48, cpu->read32(msg + 48) | 0x8); // MSG_CTRUNC
                break;
            }
            cpu->write64(control + out, 16 + dataLen);
            cpu->write32(control + out + 8, m->readd(control32 + pos + 4));
            cpu->write32(control + out + 12, m->readd(control32 + pos + 8));
            for (U32 i = 0; i < dataLen; i++) {
                cpu->write8(control + out + 16 + i, m->readb(control32 + pos + 12 + i));
            }
            out += (16 + dataLen + 7) & ~7ULL;
            pos += (len + 3) & ~3;
        }
    }
    cpu->write64(msg + 40, out);
}

static U64 x64Select(X64CPU* cpu, U64 nfds, U64 r, U64 w, U64 e, U64 timeout) {
    U32 s = scratch(cpu);
    U32 ts = 0;
    if (timeout) {
        ts = s + 4000;
        cpu->memory->writeq(ts, cpu->read64(timeout));
        cpu->memory->writeq(ts + 8, cpu->read64(timeout + 8) * 1000);
    }
    return bridge(cpu, 413, nfds, r, w, e, ts, 0); // pselect6_time64
}

static U64 x64Pselect6(X64CPU* cpu, U64 nfds, U64 r, U64 w, U64 e, U64 timeout, U64 sig) {
    U32 s = scratch(cpu);
    U32 sig32 = 0;
    if (sig) {
        sig32 = s + 4032;
        cpu->memory->writed(sig32, (U32)cpu->read64(sig));
        cpu->memory->writed(sig32 + 4, (U32)cpu->read64(sig + 8));
    }
    return bridge(cpu, 413, nfds, r, w, e, timeout, sig32);
}

static U64 x64Ppoll(X64CPU* cpu, U64 fds, U64 nfds, U64 ts) {
    S32 timeout = -1;
    if (ts) {
        U64 sec = cpu->read64(ts);
        U64 nsec = cpu->read64(ts + 8);
        U64 ms = sec * 1000 + (nsec + 999999) / 1000000;
        timeout = ms > 0x7FFFFFFF ? 0x7FFFFFFF : (S32)ms;
    }
    return bridge(cpu, 168, fds, nfds, (U32)timeout);
}

// struct flock (x86-64, 32 bytes) <-> struct flock64 (i386, 24 bytes)
static U64 x64Fcntl(X64CPU* cpu, U64 fd, U64 cmd, U64 arg) {
    bool lockCmd = (cmd >= 5 && cmd <= 7) || (cmd >= 36 && cmd <= 38);
    if (!lockCmd) {
        return bridge(cpu, 221, fd, cmd, arg);
    }
    U32 s = scratch(cpu) + 4096;
    KMemory* m = cpu->memory;
    m->writew(s, cpu->read16(arg));
    m->writew(s + 2, cpu->read16(arg + 2));
    m->writeq(s + 4, cpu->read64(arg + 8));
    m->writeq(s + 12, cpu->read64(arg + 16));
    m->writed(s + 20, cpu->read32(arg + 24));
    U32 cmd32 = (U32)cmd;
    if (cmd >= 5 && cmd <= 7) {
        cmd32 = (U32)cmd + 7; // F_GETLK64..F_SETLKW64
    }
    U32 result = (U32)bridge(cpu, 221, fd, cmd32, s);
    if (!isError(result) && (cmd == 5 || cmd == 36)) {
        cpu->write16(arg, m->readw(s));
        cpu->write16(arg + 2, m->readw(s + 2));
        cpu->write64(arg + 8, m->readq(s + 4));
        cpu->write64(arg + 16, m->readq(s + 12));
        cpu->write32(arg + 24, m->readd(s + 20));
    }
    return result;
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

static U64 x64Clone(X64CPU* cpu, U64 flags, U64 newsp, U64 ptid, U64 ctid, U64 tls) {
    KThread* thread = cpu->thread;
    KProcess* process = thread->process.get();
    if (flags & K_CLONE_THREAD) {
        KThread* newThread = process->createThread();
        X64CPU* nc = (X64CPU*)newThread->cpu;
        nc->clone64(cpu);
        nc->r[0] = 0;
        if (newsp) {
            nc->r[4] = newsp;
        }
        if (flags & K_CLONE_SETTLS) {
            nc->fsBase = tls;
        }
        newThread->sigMask = thread->sigMask;
        if (flags & K_CLONE_CHILD_CLEARTID) {
            newThread->clear_child_tid = (U32)ctid;
        }
        if ((flags & K_CLONE_PARENT_SETTID) && ptid) {
            cpu->write32(ptid, newThread->id);
        }
        if ((flags & K_CLONE_CHILD_SETTID) && ctid) {
            cpu->write32(ctid, newThread->id);
        }
        U32 id = newThread->id;
        scheduleThread(newThread);
        return id;
    }
    return process->clone(thread, (U32)flags & ~(U32)K_CLONE_SETTLS, (U32)newsp, (U32)ptid, 0, (U32)ctid);
}

static U64 x64Clone3(X64CPU* cpu, U64 args, U64 size) {
    if (size < 64) {
        return (U64)-K_EINVAL;
    }
    U64 flags = cpu->read64(args);
    U64 childTid = cpu->read64(args + 16);
    U64 parentTid = cpu->read64(args + 24);
    U64 exitSignal = cpu->read64(args + 32);
    U64 stack = cpu->read64(args + 40);
    U64 stackSize = cpu->read64(args + 48);
    U64 tls = cpu->read64(args + 56);
    return x64Clone(cpu, flags | (exitSignal & 0xFF), stack ? stack + stackSize : 0, parentTid, childTid, tls);
}

static void readStringArray64(X64CPU* cpu, U64 address, std::vector<BString>& results) {
    if (!address) {
        return;
    }
    for (U32 i = 0; i < 4096; i++) {
        U64 p = cpu->read64(address + i * 8);
        if (!p) {
            break;
        }
        results.push_back(cpu->memory->readString((U32)p));
    }
}

static U64 x64Execve(X64CPU* cpu, U64 path, U64 argv, U64 envp) {
    std::vector<BString> args;
    std::vector<BString> envs;
    BString p = cpu->memory->readString((U32)path);
    readStringArray64(cpu, argv, args);
    readStringArray64(cpu, envp, envs);
    return cpu->thread->process->execve(cpu->thread, p, args, envs);
}

static U64 x64Sigpending(X64CPU* cpu, U64 set, U64 size) {
    KThread* thread = cpu->thread;
    U64 pending = (thread->pendingSignals | thread->process->pendingSignals) & thread->sigMask;
    if (size == 4) cpu->write32(set, (U32)pending);
    else cpu->write64(set, pending);
    return 0;
}

static U64 x64Sigsuspend(X64CPU* cpu, U64 maskAddress, U64 size) {
    KThread* thread = cpu->thread;
    U64 tmpMask = size == 4 ? cpu->read32(maskAddress) : cpu->read64(maskAddress);
    U64 oldMask = thread->sigMask;
    thread->sigMask = tmpMask;
    while (!thread->terminating && !((thread->pendingSignals | thread->process->pendingSignals) & ~tmpMask)) {
        BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(thread->sigWaitCond);
        BOXEDWINE_CONDITION_WAIT_TIMEOUT(thread->sigWaitCond, 10);
    }
    // the handler runs with (and sigreturn restores) the original mask
    thread->sigMask = oldMask;
    cpu->r[0] = (U64)-K_EINTR;
    x64DeliverPendingSignal(cpu, tmpMask);
    return (U64)-K_CONTINUE;
}

// utimbuf / timeval[2] with 64-bit longs -> i386 32-bit layout
static U64 x64Utimes(X64CPU* cpu, U32 nr32, U64 path, U64 times, bool timeval) {
    U32 t = 0;
    if (times) {
        t = scratch(cpu) + 4064;
        int count = timeval ? 4 : 2;
        for (int i = 0; i < count; i++) {
            cpu->memory->writed(t + i * 4, (U32)cpu->read64(times + i * 8));
        }
    }
    return bridge(cpu, nr32, path, t);
}

static U64 x64Truncate(X64CPU* cpu, U64 path, U64 len) {
    U32 fd = (U32)bridge(cpu, 5, path, 1 /* O_WRONLY */);
    if (isError(fd)) {
        return fd;
    }
    U32 result = (U32)bridge(cpu, 194, fd, (U32)len, (U32)(len >> 32));
    bridge(cpu, 6, fd);
    return result;
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
    case 23: return x64Select(cpu, a1, a2, a3, a4, a5); // select
    case 24: return bridge(cpu, 158);                   // sched_yield
    case 25: return bridge(cpu, 163, a1, a2, a3, a4, a5); // mremap
    case 26: return bridge(cpu, 144, a1, a2, a3);       // msync
    case 27: return bridge(cpu, 218, a1, a2, a3);       // mincore
    case 28: return bridge(cpu, 219, a1, a2, a3);       // madvise
    case 29: return bridge(cpu, 395, a1, a2, a3);       // shmget
    case 30: return bridge(cpu, 397, a1, a2, a3);       // shmat
    case 31: return bridge(cpu, 396, a1, a2, a3);       // shmctl
    case 32: return bridge(cpu, 41, a1);                // dup
    case 33: return bridge(cpu, 63, a1, a2);            // dup2
    case 35: return bridge(cpu, 407, 0, 0, a1, a2);     // nanosleep -> clock_nanosleep_time64
    case 39: return bridge(cpu, 20);                    // getpid
    case 41: return bridge(cpu, 359, a1, a2, a3);       // socket
    case 42: return bridge(cpu, 362, a1, a2, a3);       // connect
    case 43: return bridge(cpu, 364, a1, a2, a3, 0);    // accept -> accept4
    case 44: return bridge(cpu, 369, a1, a2, a3, a4, a5, a6); // sendto
    case 45: return bridge(cpu, 371, a1, a2, a3, a4, a5, a6); // recvfrom
    case 46: {                                          // sendmsg
        U32 m32 = convertMsgToI386(cpu, a2, false);
        return bridge(cpu, 370, a1, m32, a3);
    }
    case 47: {                                          // recvmsg
        U32 m32 = convertMsgToI386(cpu, a2, true);
        U32 result = (U32)bridge(cpu, 372, a1, m32, a3);
        if (!isError(result)) convertRecvMsgFromI386(cpu, m32, a2);
        return result;
    }
    case 48: return bridge(cpu, 373, a1, a2);           // shutdown
    case 49: return bridge(cpu, 361, a1, a2, a3);       // bind
    case 50: return bridge(cpu, 363, a1, a2);           // listen
    case 51: return bridge(cpu, 367, a1, a2, a3);       // getsockname
    case 52: return bridge(cpu, 368, a1, a2, a3);       // getpeername
    case 53: return bridge(cpu, 360, a1, a2, a3, a4);   // socketpair
    case 54: return bridge(cpu, 366, a1, a2, a3, a4, a5); // setsockopt
    case 55: return bridge(cpu, 365, a1, a2, a3, a4, a5); // getsockopt
    case 56: return x64Clone(cpu, a1, a2, a3, a4, a5);  // clone
    case 57: return x64Clone(cpu, 17 /* SIGCHLD */, 0, 0, 0, 0); // fork
    case 58: return x64Clone(cpu, K_CLONE_VM | K_CLONE_VFORK | 17, 0, 0, 0, 0); // vfork
    case 59: return x64Execve(cpu, a1, a2, a3);         // execve
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
    case 67: return bridge(cpu, 117, 22, 0, 0, 0, a1);  // shmdt -> ipc(SHMDT)
    case 72: return x64Fcntl(cpu, a1, a2, a3);          // fcntl
    case 73: return bridge(cpu, 143, a1, a2);           // flock
    case 74: return bridge(cpu, 118, a1);               // fsync
    case 75: return bridge(cpu, 148, a1);               // fdatasync
    case 76: return x64Truncate(cpu, a1, a2);           // truncate
    case 77: return bridge(cpu, 194, a1, (U32)a2, (U32)(a2 >> 32)); // ftruncate -> ftruncate64
    case 79: return bridge(cpu, 183, a1, a2);           // getcwd
    case 80: return bridge(cpu, 12, a1);                // chdir
    case 81: return bridge(cpu, 133, a1);               // fchdir
    case 82: return bridge(cpu, 38, a1, a2);            // rename
    case 83: return bridge(cpu, 39, a1, a2);            // mkdir
    case 84: return bridge(cpu, 40, a1);                // rmdir
    case 85: return bridge(cpu, 5, a1, 0x241, a2);      // creat -> open(O_CREAT|O_WRONLY|O_TRUNC)
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
    case 127: return x64Sigpending(cpu, a1, a2);        // rt_sigpending
    case 128: return bridge(cpu, 421, a1, a2, a3, a4);  // rt_sigtimedwait -> time64
    case 130: return x64Sigsuspend(cpu, a1, a2);        // rt_sigsuspend
    case 132: return x64Utimes(cpu, 30, a1, a2, false); // utime
    case 140: return 20;                                // getpriority (nice 0)
    case 141: return bridge(cpu, 97, a1, a2, a3);       // setpriority
    case 143: return bridge(cpu, 155, a1, a2);          // sched_getparam
    case 145: return bridge(cpu, 157, a1);              // sched_getscheduler
    case 146: return bridge(cpu, 159, a1);              // sched_get_priority_max
    case 147: return bridge(cpu, 160, a1);              // sched_get_priority_min
    case 149: return bridge(cpu, 150, a1, a2);          // mlock
    case 150: return bridge(cpu, 151, a1, a2);          // munlock
    case 162: return bridge(cpu, 36);                   // sync
    case 131: return sigaltstack64(cpu, a1, a2);        // sigaltstack
    case 137: case 138: {                               // statfs, fstatfs
        U32 s = scratch(cpu);
        U32 result = (U32)bridge(cpu, nr == 137 ? 268 : 269, a1, 84, s);
        if (!isError(result)) convertStatfs(cpu, s, a2);
        return result;
    }
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
    case 221: return 0;                                 // fadvise64 (advisory)
    case 235: return x64Utimes(cpu, 271, a1, a2, true);  // utimes
    case 253: return bridge(cpu, 291);                  // inotify_init
    case 254: return bridge(cpu, 292, a1, a2, a3);      // inotify_add_watch
    case 255: return bridge(cpu, 293, a1, a2);          // inotify_rm_watch
    case 263: return bridge(cpu, 301, a1, a2, a3);      // unlinkat
    case 264: return bridge(cpu, 302, a1, a2, a3, a4);  // renameat
    case 266: return bridge(cpu, 304, a1, a2, a3);      // symlinkat
    case 267: return bridge(cpu, 305, a1, a2, a3, a4);  // readlinkat
    case 268: return bridge(cpu, 306, a1, a2, a3);      // fchmodat
    case 269: return bridge(cpu, 307, a1, a2, a3);      // faccessat
    case 270: return x64Pselect6(cpu, a1, a2, a3, a4, a5, a6); // pselect6
    case 271: return x64Ppoll(cpu, a1, a2, a3);         // ppoll (sigmask ignored)
    case 281: return bridge(cpu, 256, a1, a2, a3, a4);  // epoll_pwait (sigmask ignored)
    case 277: return 0;                                 // sync_file_range
    case 285: return bridge(cpu, 324, a1, a2, (U32)a3, (U32)(a3 >> 32), (U32)a4, (U32)(a4 >> 32)); // fallocate
    case 288: return bridge(cpu, 364, a1, a2, a3, a4);  // accept4
    case 273: return 0;                                 // set_robust_list (robust futexes not emulated)
    case 280: return bridge(cpu, 412, a1, a2, a3, a4);  // utimensat -> time64
    case 284: return bridge(cpu, 328, a1, 0);           // eventfd
    case 289: return bridge(cpu, 327, a1, a2, a3, a4);  // signalfd4
    case 290: return bridge(cpu, 328, a1, a2);          // eventfd2
    case 291: return bridge(cpu, 329, a1);              // epoll_create1
    case 292: return bridge(cpu, 330, a1, a2, a3);      // dup3
    case 293: return bridge(cpu, 331, a1, a2);          // pipe2
    case 294: return bridge(cpu, 291);                  // inotify_init1 (flags ignored)
    case 302: return bridge(cpu, 340, a1, a2, a3, a4);  // prlimit64
    case 316: return bridge(cpu, 353, a1, a2, a3, a4, a5); // renameat2
    case 309: {                                         // getcpu
        if (a1) cpu->write32(a1, 0);
        if (a2) cpu->write32(a2, 0);
        return 0;
    }
    case 318: return bridge(cpu, 355, a1, a2, a3);      // getrandom
    case 319: return bridge(cpu, 356, a1, a2);          // memfd_create
    case 332: return bridge(cpu, 383, a1, a2, a3, a4, a5); // statx
    case 334: return (U64)-K_ENOSYS;                    // rseq
    case 435: return x64Clone3(cpu, a1, a2);            // clone3
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
    if (cpu->thread->startSignal && !cpu->thread->terminating) {
        // a signal interrupted a blocking call: restart it after the handler
        cpu->thread->startSignal = false;
        if (r32 != (U32)-K_CONTINUE) {
            cpu->rip = startRip - 2;
            x64DeliverPendingSignal(cpu, cpu->thread->sigMask);
            return;
        }
    }
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
    if (cpu->thread->pendingSignals | cpu->thread->process->pendingSignals) {
        x64DeliverPendingSignal(cpu, cpu->thread->sigMask);
    }
}
