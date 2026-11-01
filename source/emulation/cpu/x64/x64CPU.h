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

#ifndef __X64_CPU_H__
#define __X64_CPU_H__

#include "../common/cpu.h"

// Experimental x86-64 (long mode) guest CPU.
//
// This is a portable interpreter that runs 64-bit user mode code on top of the
// existing BoxedWine kernel. The guest keeps every mapping below 4GB, so the
// existing 32-bit KMemory, mmap and file APIs can be reused unchanged; only the
// register file, decoder, ELF loader and syscall ABI are 64-bit.

union X64Xmm {
    U8 b[16];
    U16 w[8];
    U32 d[4];
    U64 q[2];
    S8 sb[16];
    S16 sw[8];
    S32 sd[4];
    S64 sq[2];
    float f[4];
    double fd[2];
};

class X64CPU : public CPU {
public:
    X64CPU(KMemory* memory);

    // from CPU
    void run() override;
    void reset() override;
    // The x64 interpreter does not use DecodedOps; the platform thread loop only
    // needs a non-null op to know the thread can start.
    DecodedOp* getOp(U32 eip, U32 flags) override;
    bool isValidExecutableAddress(U32 address) override { return true; }

    void clone64(X64CPU* from);
    void dumpState();

    U64 r[16] = {};
    U64 rip = 0;
    U64 fsBase = 0;
    U64 gsBase = 0;
    ALIGN(X64Xmm x[16], 16);

    // Arithmetic flags are computed lazily from the last flag-setting op.
    U32 lfOp = 0;
    U32 lfBits = 32;
    U64 lfDst = 0;
    U64 lfSrc = 0;
    U64 lfRes = 0;
    U32 lfCarryIn = 0;
    U32 rflags = 0x202; // materialized flags; CF..OF only valid when lfOp==0

    U32 getFlags();
    void setFlagsValue(U32 value);
    bool getCFBit();

    void push64(U64 value);
    U64 pop64();

    // set by the syscall layer
    bool syscallInvalidatedFetch = false;
    void invalidateFetchCache() { fetchPage = ~0ULL; }
    void restartInstruction() { rip = instStart; }

    // guest page used by the syscall layer to stage i386 structs (0 = not allocated yet)
    U32 scratchPage = 0;

    // 64-bit signal frame support
    U64 sigAltStack = 0;
    U64 sigAltStackSize = 0;

    // memory helpers (guest addresses must be below 4GB)
    U8 read8(U64 address);
    U16 read16(U64 address);
    U32 read32(U64 address);
    U64 read64(U64 address);
    void write8(U64 address, U8 value);
    void write16(U64 address, U16 value);
    void write32(U64 address, U32 value);
    void write64(U64 address, U64 value);
    void readXmm(U64 address, X64Xmm& value);
    void writeXmm(U64 address, const X64Xmm& value);

private:
    // decode state for the current instruction
    U64 instStart = 0;
    U8 rex = 0;
    bool opSize = false;
    bool adSize = false;
    U8 repPrefix = 0;
    U8 segPrefix = 0;
    U8 modrm = 0;
    U8 mod = 0;
    U8 regField = 0;
    U8 rmField = 0;
    bool isMem = false;
    bool ripRelative = false;
    U64 eaPartial = 0;

    U64 fetchPage = ~0ULL;
    U8* fetchPtr = nullptr;

    void step();
    void step0F();
    void step0F38();
    void step0F3A();
    bool stepSSE(U8 op);
    void stepFPU(U8 op);
    void unhandled(const char* what, U32 op);
    void fault(U64 address, bool write);
    void raiseException(int signal, int code);

    U8 fetch8();
    U16 fetch16();
    U32 fetch32();
    U64 fetch64();
    void refillFetch(U64 address);

    void decodeModrm();
    U64 ea();
    int opBits() const { return (rex & 8) ? 64 : (opSize ? 16 : 32); }

    U64 getReg(int reg, int bits);
    void setReg(int reg, int bits, U64 value);
    U64 readRM(int bits);
    void writeRM(int bits, U64 value);

    // LOCK prefix: atomic load in readRM, compare-and-swap in writeRM; on a
    // lost race the whole instruction is retried.
    bool lockPrefix = false;
    bool lockRetry = false;
    U8* lockPtr = nullptr;
    U64 lockOld = 0;
    U64 lockedRead(U64 address, int bits);
    void lockedWrite(U64 address, int bits, U64 value);
    U64 readMem(U64 address, int bits);
    void writeMem(U64 address, int bits, U64 value);

    void setLazy(U32 op, int bits, U64 dst, U64 src, U64 res);
    void setLogic(int bits, U64 res);
    U64 alu(int op, int bits, U64 a, U64 b);
    U64 shiftOp(int op, int bits, U64 value, U32 count);
    void group3(int bits);
    bool cond(int cc);
    void doString(U8 op);
    void doSyscall();
    void doCpuid();
    U64 shld(int bits, U64 dst, U64 src, U32 count, bool left);
};

void ksyscall64(X64CPU* cpu);
// delivers the lowest pending signal not blocked by mask; returns true if one was delivered
bool x64DeliverPendingSignal(X64CPU* cpu, U64 mask);
void x64SyncMxcsrToHost(X64CPU* cpu);
void x64SyncMxcsrFromHost(X64CPU* cpu);
void x64RunSignal(X64CPU* cpu, U32 signal);
U64 x64Sigreturn(X64CPU* cpu);

#endif
