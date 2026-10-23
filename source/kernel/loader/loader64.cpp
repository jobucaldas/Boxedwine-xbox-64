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
#include "loader.h"
#include "loader64.h"
#include "ksignal.h"
#include "../../emulation/cpu/x64/x64CPU.h"

// ELF64 (x86-64) loading, initial stack and signal frames for the experimental
// 64-bit guest mode. Every segment must fit below 4GB (static non-PIE binaries
// link at 0x400000; ET_DYN images are placed at ADDRESS_PROCESS_LOADER).

#define K_PT_LOAD 1
#define K_ET_DYN 3
#define K_EM_X86_64 62

struct k_Elf64_Ehdr {
    U8 e_ident[16];
    U16 e_type;
    U16 e_machine;
    U32 e_version;
    U64 e_entry;
    U64 e_phoff;
    U64 e_shoff;
    U32 e_flags;
    U16 e_ehsize;
    U16 e_phentsize;
    U16 e_phnum;
    U16 e_shentsize;
    U16 e_shnum;
    U16 e_shstrndx;
};

struct k_Elf64_Phdr {
    U32 p_type;
    U32 p_flags;
    U64 p_offset;
    U64 p_vaddr;
    U64 p_paddr;
    U64 p_filesz;
    U64 p_memsz;
    U64 p_align;
};

bool isElf64X64(const U8* header, U32 len) {
    if (len < 20) {
        return false;
    }
    if (header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        return false;
    }
    U16 machine = header[18] | (header[19] << 8);
    return header[4] == 2 && header[5] == 1 && machine == K_EM_X86_64;
}

BString getInterpreter64(FsOpenNode* openNode) {
    k_Elf64_Ehdr hdr;
    openNode->seek(0);
    if (openNode->readNative((U8*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
        return B("");
    }
    for (U32 i = 0; i < hdr.e_phnum; i++) {
        k_Elf64_Phdr phdr;
        openNode->seek(hdr.e_phoff + (U64)i * hdr.e_phentsize);
        openNode->readNative((U8*)&phdr, sizeof(phdr));
        if (phdr.p_type == 3 && phdr.p_filesz < MAX_FILEPATH_LEN) { // PT_INTERP
            char interp[MAX_FILEPATH_LEN] = { 0 };
            openNode->seek(phdr.p_offset);
            openNode->readNative((U8*)interp, (U32)phdr.p_filesz);
            interp[phdr.p_filesz] = 0;
            return BString::copy(interp);
        }
    }
    return B("");
}

bool loadProgram64(KThread* thread, FsOpenNode* openNode) {
    k_Elf64_Ehdr hdr;
    openNode->seek(0);
    if (openNode->readNative((U8*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
        return false;
    }
    if (!isElf64X64(hdr.e_ident, 20) && hdr.e_machine != K_EM_X86_64) {
        return false;
    }
    U64 low = ~0ULL;
    U64 high = 0;
    for (U32 i = 0; i < hdr.e_phnum; i++) {
        k_Elf64_Phdr phdr;
        openNode->seek(hdr.e_phoff + (U64)i * hdr.e_phentsize);
        openNode->readNative((U8*)&phdr, sizeof(phdr));
        if (phdr.p_type == K_PT_LOAD) {
            if (phdr.p_vaddr < low) low = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > high) high = phdr.p_vaddr + phdr.p_memsz;
        }
    }
    if (low == ~0ULL) {
        return false;
    }
    low &= ~0xFFFULL;
    high = (high + 0xFFF) & ~0xFFFULL;

    U64 reloc = 0;
    if (hdr.e_type == K_ET_DYN && low < 0x10000) {
        reloc = ((U64)ADDRESS_PROCESS_LOADER << K_PAGE_SHIFT) - low;
    }
    if ((high + reloc) > 0x100000000ULL) {
        klog_fmt("x64: ELF image %llx-%llx does not fit in the 4GB guest window", (unsigned long long)(low + reloc), (unsigned long long)(high + reloc));
        return false;
    }
    U32 base = (U32)(low + reloc);
    U32 len = (U32)(high - low);
    // map the whole image once so segments sharing a page don't wipe each other
    U32 address = thread->memory->mmap(thread, base, len, K_PROT_READ | K_PROT_WRITE | K_PROT_EXEC, K_MAP_PRIVATE | K_MAP_ANONYMOUS | K_MAP_FIXED, -1, 0);
    if (address != base) {
        klog_fmt("x64: could not map ELF image at %X", base);
        return false;
    }
    KProcess* process = thread->process.get();
    process->loaderBaseAddress = reloc ? base : 0;
    process->brkEnd = base + len;
    process->phdr = 0;

    for (U32 i = 0; i < hdr.e_phnum; i++) {
        k_Elf64_Phdr phdr;
        openNode->seek(hdr.e_phoff + (U64)i * hdr.e_phentsize);
        openNode->readNative((U8*)&phdr, sizeof(phdr));
        if (phdr.p_type == K_PT_LOAD && phdr.p_filesz > 0) {
            if (phdr.p_offset <= hdr.e_phoff && hdr.e_phoff < phdr.p_offset + phdr.p_filesz) {
                process->phdr = (U32)(reloc + phdr.p_vaddr + hdr.e_phoff - phdr.p_offset);
            }
            openNode->seek(phdr.p_offset);
            openNode->read(thread, (U32)(reloc + phdr.p_vaddr), (U32)phdr.p_filesz);
        }
    }
    process->phentsize = hdr.e_phentsize;
    process->phnum = hdr.e_phnum;
    process->entry = (U32)(hdr.e_entry + reloc);
    process->isX64 = true;
    return true;
}

static U64 pushString(X64CPU* cpu, const char* s) {
    U64 len = strlen(s) + 1;
    cpu->r[4] -= len;
    cpu->memory->memcpy((U32)cpu->r[4], s, (U32)len);
    return cpu->r[4];
}

void x64ReplaceThreadCpu(KThread* thread) {
    CPU* old = thread->cpu;
    CPU* cpu = CPU::allocCPU(thread->memory);
    cpu->thread = thread;
    cpu->reg[4].u32 = old->reg[4].u32;
    cpu->eip.u32 = old->eip.u32;
    if (thread->process->isX64) {
        ((X64CPU*)cpu)->r[4] = old->reg[4].u32;
    }
    thread->cpu = cpu;
    delete thread->retiredCpu;
    thread->retiredCpu = old;
}

void x64CloneCpu(KThread* thread, KThread* from) {
    X64CPU* cpu = (X64CPU*)thread->cpu;
    cpu->clone64((X64CPU*)from->cpu);
    cpu->sigAltStack = ((X64CPU*)from->cpu)->sigAltStack;
    cpu->sigAltStackSize = ((X64CPU*)from->cpu)->sigAltStackSize;
}

void x64ForkChild(KThread* thread, U32 childStack) {
    X64CPU* cpu = (X64CPU*)thread->cpu;
    cpu->r[0] = 0;
    if (childStack) {
        cpu->r[4] = childStack;
    }
}

void x64SetupMainThread(KThread* thread, const std::vector<BString>& args, const std::vector<BString>& env) {
    KProcess* process = thread->process.get();
    X64CPU* cpu = (X64CPU*)thread->cpu;
    cpu->r[4] = cpu->reg[4].u32; // top of the stack from KThread::setupStack
    cpu->rip = process->entry;

    U64 randomBytes[2] = { ((U64)rand() << 32) ^ rand(), ((U64)rand() << 32) ^ rand() };
    cpu->r[4] -= 16;
    cpu->memory->memcpy((U32)cpu->r[4], randomBytes, 16);
    U64 random = cpu->r[4];
    U64 platform = pushString(cpu, "x86_64");
    U64 execfn = pushString(cpu, args.size() ? args[0].c_str() : "");
    std::vector<U64> envp;
    std::vector<U64> argv;
    for (size_t i = 0; i < env.size(); i++) {
        envp.push_back(pushString(cpu, env[i].c_str()));
        if (strncmp(env[i].c_str(), "PATH=", 5) == 0) {
            env[i].substr(5).split(':', process->path);
        }
    }
    for (size_t i = 0; i < args.size(); i++) {
        argv.push_back(pushString(cpu, args[i].c_str()));
    }
    cpu->r[4] &= ~0xFULL;

    // hwcap mirrors CPUID.1:EDX reported by the interpreter
    U64 hwcap = (1 << 0) | (1 << 4) | (1 << 8) | (1 << 15) | (1 << 19) | (1 << 23) | (1 << 24) | (1 << 25) | (1 << 26);
    U64 auxv[][2] = {
        { 3, process->phdr }, { 4, process->phentsize }, { 5, process->phnum }, { 6, K_PAGE_SIZE },
        { 7, process->loaderBaseAddress }, { 8, 0 }, { 9, process->entry },
        { 11, (U64)process->userId }, { 12, (U64)process->effectiveUserId }, { 13, (U64)process->groupId }, { 14, (U64)process->effectiveGroupId },
        { 15, platform }, { 16, hwcap }, { 17, 100 }, { 23, 0 }, { 25, random }, { 26, 0 }, { 31, execfn }, { 0, 0 }
    };
    U32 auxCount = sizeof(auxv) / sizeof(auxv[0]);
    U64 words = 1 + argv.size() + 1 + envp.size() + 1 + auxCount * 2;
    if (words & 1) {
        cpu->push64(0);
    }
    for (int i = (int)auxCount - 1; i >= 0; i--) {
        cpu->push64(auxv[i][1]);
        cpu->push64(auxv[i][0]);
    }
    cpu->push64(0);
    for (int i = (int)envp.size() - 1; i >= 0; i--) {
        cpu->push64(envp[i]);
    }
    cpu->push64(0);
    for (int i = (int)argv.size() - 1; i >= 0; i--) {
        cpu->push64(argv[i]);
    }
    cpu->push64(argv.size());
    cpu->r[2] = 0; // rdx: no rtld fini function
}

// ---------------------------------------------------------------- signals

#define UC_OFFSET 8
#define MCONTEXT_OFFSET (UC_OFFSET + 40)
#define SIGMASK_OFFSET (UC_OFFSET + 296)
#define SIGINFO_OFFSET (UC_OFFSET + 304)
#define FRAME_SIZE (SIGINFO_OFFSET + 128)

// sigcontext register order
static const int sigcontextRegs[16] = { 8, 9, 10, 11, 12, 13, 14, 15, 7, 6, 5, 3, 2, 0, 1, 4 };

void x64RunSignal(X64CPU* cpu, U32 signal) {
    KThread* thread = cpu->thread;
    KProcess* process = thread->process.get();
    KSigAction* action = &process->sigActions[signal];
    bool synchronous = signal == K_SIGSEGV || signal == K_SIGILL || signal == K_SIGFPE || signal == K_SIGBUS || signal == K_SIGTRAP;

    if (action->handlerAndSigAction == K_SIG_DFL || (action->handlerAndSigAction == K_SIG_IGN && synchronous)) {
        if (signal == K_SIGURG || signal == K_SIGCONT || signal == K_SIGCHLD || signal == K_SIGWINCH) {
            return;
        }
        klog_fmt("x64: process %d terminated by signal %d", process->id, signal);
        process->signal(K_SIGKILL);
        return;
    }
    if (action->handlerAndSigAction == K_SIG_IGN) {
        return;
    }
    U64 sp = cpu->r[4];
    bool onAlt = cpu->sigAltStack && sp > cpu->sigAltStack && sp <= cpu->sigAltStack + cpu->sigAltStackSize;
    if ((action->flags & K_SA_ONSTACK) && cpu->sigAltStack && !onAlt) {
        sp = cpu->sigAltStack + cpu->sigAltStackSize;
    } else {
        sp -= 128; // red zone
    }
    sp -= 512;
    sp &= ~63ULL;
    U64 fpstate = sp;
    sp -= FRAME_SIZE;
    sp = (sp & ~15ULL) - 8;
    U64 frame = sp;

    // fxsave-format fp state
    for (int i = 0; i < 512; i += 8) cpu->write64(fpstate + i, 0);
    cpu->write16(fpstate, cpu->fpu.CW());
    cpu->write16(fpstate + 2, cpu->fpu.SW());
    cpu->write32(fpstate + 24, cpu->mxcsr);
    cpu->write32(fpstate + 28, 0xFFFF);
    for (int i = 0; i < 16; i++) cpu->writeXmm(fpstate + 160 + i * 16, cpu->x[i]);

    for (U32 i = 0; i < FRAME_SIZE; i += 8) cpu->write64(frame + i, 0);
    cpu->write64(frame, action->restorer);
    cpu->write64(frame + UC_OFFSET, 0);  // uc_flags
    cpu->write64(frame + UC_OFFSET + 8, 0); // uc_link
    cpu->write64(frame + UC_OFFSET + 16, cpu->sigAltStack);
    cpu->write32(frame + UC_OFFSET + 24, cpu->sigAltStack ? (onAlt ? 1 : 0) : 2);
    cpu->write64(frame + UC_OFFSET + 32, cpu->sigAltStackSize);
    U64 mc = frame + MCONTEXT_OFFSET;
    for (int i = 0; i < 16; i++) cpu->write64(mc + i * 8, cpu->r[sigcontextRegs[i]]);
    cpu->write64(mc + 16 * 8, cpu->rip);
    cpu->write64(mc + 17 * 8, cpu->getFlags());
    cpu->write16(mc + 18 * 8, 0x33); // cs
    cpu->write16(mc + 18 * 8 + 6, 0x2b); // ss
    cpu->write64(mc + 19 * 8, 0); // err
    cpu->write64(mc + 20 * 8, signal == K_SIGSEGV ? 14 : 0); // trapno
    cpu->write64(mc + 21 * 8, thread->sigMask); // oldmask
    cpu->write64(mc + 22 * 8, signal == K_SIGSEGV ? action->sigInfo[3] : 0); // cr2
    cpu->write64(mc + 23 * 8, fpstate);
    cpu->write64(frame + SIGMASK_OFFSET, thread->sigMask);

    U64 info = frame + SIGINFO_OFFSET;
    cpu->write32(info, signal);
    cpu->write32(info + 4, action->sigInfo[1]);
    cpu->write32(info + 8, action->sigInfo[2]);
    if (synchronous) {
        cpu->write64(info + 16, action->sigInfo[3]);
    } else {
        cpu->write32(info + 16, action->sigInfo[3]);
        cpu->write32(info + 20, action->sigInfo[4]);
    }

    U64 newMask = thread->sigMask | action->mask;
    if (!(action->flags & K_SA_NODEFER)) {
        newMask |= 1ULL << (signal - 1);
    }
    U32 handler = action->handlerAndSigAction;
    if (action->flags & K_SA_RESETHAND) {
        action->handlerAndSigAction = K_SIG_DFL;
    }
    thread->sigMask = newMask;
    cpu->r[7] = signal;
    cpu->r[6] = info;
    cpu->r[2] = frame + UC_OFFSET;
    cpu->r[0] = 0;
    cpu->r[4] = frame;
    cpu->rip = handler;
    cpu->setFlagsValue(cpu->getFlags() & ~(DF | TF));
    cpu->invalidateFetchCache();
}

U64 x64Sigreturn(X64CPU* cpu) {
    U64 uc = cpu->r[4];
    U64 mc = uc + 40;
    U64 regs[16];
    for (int i = 0; i < 16; i++) regs[i] = cpu->read64(mc + i * 8);
    for (int i = 0; i < 16; i++) cpu->r[sigcontextRegs[i]] = regs[i];
    cpu->rip = cpu->read64(mc + 16 * 8);
    cpu->setFlagsValue((U32)cpu->read64(mc + 17 * 8));
    U64 fpstate = cpu->read64(mc + 23 * 8);
    if (fpstate) {
        cpu->fpu.SetCW(cpu->read16(fpstate));
        cpu->mxcsr = cpu->read32(fpstate + 24);
        for (int i = 0; i < 16; i++) cpu->readXmm(fpstate + 160 + i * 16, cpu->x[i]);
    }
    cpu->thread->sigMask = cpu->read64(uc + 296);
    cpu->invalidateFetchCache();
    return cpu->r[0];
}

void x64SegFault(KThread* thread, U32 address, bool accessError) {
    X64CPU* cpu = (X64CPU*)thread->cpu;
    KSigAction* action = &thread->process->sigActions[K_SIGSEGV];
    action->sigInfo[0] = K_SIGSEGV;
    action->sigInfo[1] = 0;
    action->sigInfo[2] = accessError ? 2 : 1;
    action->sigInfo[3] = address;
    if (action->handlerAndSigAction == K_SIG_DFL || action->handlerAndSigAction == K_SIG_IGN) {
        klog_fmt("x64: segmentation fault accessing %08X", address);
        cpu->dumpState();
    }
    cpu->restartInstruction();
    x64RunSignal(cpu, K_SIGSEGV);
    throw 4;
}
