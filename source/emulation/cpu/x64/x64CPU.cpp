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
#include "x64CPU.h"
#include "x64Math.h"
#include "x64Atomic.h"
#include "ksignal.h"

enum {
    LF_NONE = 0,
    LF_ADD,
    LF_ADC,
    LF_SUB,
    LF_SBB,
    LF_LOGIC,
    LF_INC,
    LF_DEC,
    LF_NEG
};

#define FLAGS_ARITH (CF | PF | AF | ZF | SF | OF)
#define RSP_INDEX 4

static U8 parityTable[256];

static void initParityTable() {
    for (U32 i = 0; i < 256; i++) {
        U32 bits = 0;
        for (U32 b = 0; b < 8; b++) {
            bits += (i >> b) & 1;
        }
        parityTable[i] = (bits & 1) ? 0 : PF;
    }
}

static inline U64 maskFor(int bits) {
    return bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
}

static inline U64 signFor(int bits) {
    return 1ULL << (bits - 1);
}

static inline S64 signExtend(U64 value, int bits) {
    if (bits == 64) {
        return (S64)value;
    }
    U64 m = signFor(bits);
    value &= maskFor(bits);
    return (S64)((value ^ m) - m);
}

X64CPU::X64CPU(KMemory* memory) : CPU(memory) {
    static bool parityInit = false;
    if (!parityInit) {
        initParityTable();
        parityInit = true;
    }
    memset(x, 0, sizeof(x));
    big = 1;
}

DecodedOp* X64CPU::getOp(U32 eip, U32 flags) {
    static DecodedOp placeholder;
    return &placeholder;
}

void X64CPU::reset() {
    CPU::reset();
    memset(r, 0, sizeof(r));
    memset(x, 0, sizeof(x));
    rip = 0;
    fsBase = 0;
    gsBase = 0;
    lfOp = LF_NONE;
    rflags = 0x202;
    mxcsr = 0x1F80;
    invalidateFetchCache();
}

void X64CPU::clone64(X64CPU* from) {
    memcpy(r, from->r, sizeof(r));
    memcpy(x, from->x, sizeof(x));
    rip = from->rip;
    fsBase = from->fsBase;
    gsBase = from->gsBase;
    lfOp = from->lfOp;
    lfBits = from->lfBits;
    lfDst = from->lfDst;
    lfSrc = from->lfSrc;
    lfRes = from->lfRes;
    lfCarryIn = from->lfCarryIn;
    rflags = from->rflags;
    mxcsr = from->mxcsr;
    fpu = from->fpu;
    invalidateFetchCache();
}

void X64CPU::dumpState() {
    static const char* names[16] = { "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8 ", "R9 ", "R10", "R11", "R12", "R13", "R14", "R15" };
    klog_fmt("x64 RIP=%016llx (instruction start %016llx) FLAGS=%08x FS=%016llx", (unsigned long long)rip, (unsigned long long)instStart, getFlags(), (unsigned long long)fsBase);
    for (int i = 0; i < 16; i += 4) {
        klog_fmt("  %s=%016llx %s=%016llx %s=%016llx %s=%016llx", names[i], (unsigned long long)r[i], names[i + 1], (unsigned long long)r[i + 1], names[i + 2], (unsigned long long)r[i + 2], names[i + 3], (unsigned long long)r[i + 3]);
    }
    char bytes[64] = { 0 };
    int pos = 0;
    for (int i = 0; i < 15; i++) {
        U64 a = instStart + i;
        if (a >> 32) {
            break;
        }
        pos += snprintf(bytes + pos, sizeof(bytes) - pos, "%02x ", memory->readb((U32)a));
    }
    klog_fmt("  bytes at instruction: %s", bytes);
}

// ---------------------------------------------------------------- flags

U32 X64CPU::getFlags() {
    if (lfOp != LF_NONE) {
        U64 m = maskFor(lfBits);
        U64 s = signFor(lfBits);
        U64 d = lfDst & m;
        U64 sr = lfSrc & m;
        U64 res = lfRes & m;
        bool cf = false, of = false, af = false;

        switch (lfOp) {
        case LF_ADD:
            cf = res < d;
            of = ((d ^ res) & (sr ^ res) & s) != 0;
            af = ((d ^ sr ^ res) & 0x10) != 0;
            break;
        case LF_ADC:
            cf = lfCarryIn ? res <= d : res < d;
            of = ((d ^ res) & (sr ^ res) & s) != 0;
            af = ((d ^ sr ^ res) & 0x10) != 0;
            break;
        case LF_SUB:
            cf = d < sr;
            of = ((d ^ sr) & (d ^ res) & s) != 0;
            af = ((d ^ sr ^ res) & 0x10) != 0;
            break;
        case LF_SBB:
            cf = lfCarryIn ? d <= sr : d < sr;
            of = ((d ^ sr) & (d ^ res) & s) != 0;
            af = ((d ^ sr ^ res) & 0x10) != 0;
            break;
        case LF_LOGIC:
            break;
        case LF_INC:
            cf = lfCarryIn != 0;
            of = res == s;
            af = (res & 0xf) == 0;
            break;
        case LF_DEC:
            cf = lfCarryIn != 0;
            of = res == s - 1;
            af = (res & 0xf) == 0xf;
            break;
        case LF_NEG:
            cf = sr != 0;
            of = res == s;
            af = ((sr ^ res) & 0x10) != 0;
            break;
        }
        U32 f = rflags & ~FLAGS_ARITH;
        if (cf) f |= CF;
        if (of) f |= OF;
        if (af) f |= AF;
        if (res == 0) f |= ZF;
        if (res & s) f |= SF;
        f |= parityTable[res & 0xff];
        rflags = f;
        lfOp = LF_NONE;
    }
    return rflags;
}

void X64CPU::setFlagsValue(U32 value) {
    lfOp = LF_NONE;
    rflags = (value & (FLAGS_ARITH | DF | TF | AC | ID)) | 0x202;
}

bool X64CPU::getCFBit() {
    return (getFlags() & CF) != 0;
}

void X64CPU::setLazy(U32 op, int bits, U64 dst, U64 src, U64 res) {
    lfOp = op;
    lfBits = bits;
    lfDst = dst;
    lfSrc = src;
    lfRes = res;
}

void X64CPU::setLogic(int bits, U64 res) {
    setLazy(LF_LOGIC, bits, 0, 0, res);
}

bool X64CPU::cond(int cc) {
    bool result;
    if (lfOp == LF_SUB || lfOp == LF_LOGIC) {
        // fast paths for cmp/test followed by a branch
        U64 m = maskFor(lfBits);
        U64 d = lfDst & m, s = lfSrc & m, res = lfRes & m;
        switch (cc >> 1) {
        case 1: // B
            result = lfOp == LF_SUB ? d < s : false;
            return (cc & 1) ? !result : result;
        case 2: // Z
            result = res == 0;
            return (cc & 1) ? !result : result;
        case 3: // BE
            result = (lfOp == LF_SUB ? d < s : false) || res == 0;
            return (cc & 1) ? !result : result;
        case 6: // L
            result = lfOp == LF_SUB ? signExtend(d, lfBits) < signExtend(s, lfBits) : (res & signFor(lfBits)) != 0;
            return (cc & 1) ? !result : result;
        case 7: // LE
            result = lfOp == LF_SUB ? signExtend(d, lfBits) <= signExtend(s, lfBits) : ((res & signFor(lfBits)) != 0 || res == 0);
            return (cc & 1) ? !result : result;
        default:
            break;
        }
    }
    U32 f = getFlags();
    switch (cc >> 1) {
    case 0: result = (f & OF) != 0; break;
    case 1: result = (f & CF) != 0; break;
    case 2: result = (f & ZF) != 0; break;
    case 3: result = (f & (CF | ZF)) != 0; break;
    case 4: result = (f & SF) != 0; break;
    case 5: result = (f & PF) != 0; break;
    case 6: result = ((f & SF) != 0) != ((f & OF) != 0); break;
    default: result = (f & ZF) != 0 || (((f & SF) != 0) != ((f & OF) != 0)); break;
    }
    return (cc & 1) ? !result : result;
}

// ---------------------------------------------------------------- memory

void X64CPU::fault(U64 address, bool write) {
    if (address >> 32) {
        klog_fmt("x64: access to %016llx is outside the 4GB guest window", (unsigned long long)address);
        dumpState();
        raiseException(K_SIGSEGV, 0);
        return;
    }
}

U8 X64CPU::read8(U64 address) {
    if (address >> 32) fault(address, false);
    return memory->readb((U32)address);
}

U16 X64CPU::read16(U64 address) {
    if (address >> 32) fault(address, false);
    return memory->readw((U32)address);
}

U32 X64CPU::read32(U64 address) {
    if (address >> 32) fault(address, false);
    return memory->readd((U32)address);
}

U64 X64CPU::read64(U64 address) {
    if (address >> 32) fault(address, false);
    return memory->readq((U32)address);
}

void X64CPU::write8(U64 address, U8 value) {
    if (address >> 32) fault(address, true);
    memory->writeb((U32)address, value);
}

void X64CPU::write16(U64 address, U16 value) {
    if (address >> 32) fault(address, true);
    memory->writew((U32)address, value);
}

void X64CPU::write32(U64 address, U32 value) {
    if (address >> 32) fault(address, true);
    memory->writed((U32)address, value);
}

void X64CPU::write64(U64 address, U64 value) {
    if (address >> 32) fault(address, true);
    memory->writeq((U32)address, value);
}

void X64CPU::readXmm(U64 address, X64Xmm& value) {
    value.q[0] = read64(address);
    value.q[1] = read64(address + 8);
}

void X64CPU::writeXmm(U64 address, const X64Xmm& value) {
    write64(address, value.q[0]);
    write64(address + 8, value.q[1]);
}

U64 X64CPU::readMem(U64 address, int bits) {
    switch (bits) {
    case 8: return read8(address);
    case 16: return read16(address);
    case 32: return read32(address);
    default: return read64(address);
    }
}

void X64CPU::writeMem(U64 address, int bits, U64 value) {
    switch (bits) {
    case 8: write8(address, (U8)value); break;
    case 16: write16(address, (U16)value); break;
    case 32: write32(address, (U32)value); break;
    default: write64(address, value); break;
    }
}

void X64CPU::push64(U64 value) {
    U64 sp = r[RSP_INDEX] - 8;
    write64(sp, value);
    r[RSP_INDEX] = sp;
}

U64 X64CPU::pop64() {
    U64 value = read64(r[RSP_INDEX]);
    r[RSP_INDEX] += 8;
    return value;
}

// ---------------------------------------------------------------- fetch / decode

void X64CPU::refillFetch(U64 address) {
    U64 page = address & ~0xFFFULL;
    if (address >> 32) {
        fault(address, false);
    }
    U8* p = memory->getRamPtr((U32)page, K_PAGE_SIZE, false);
    if (!p || !(memory->getPageFlags((U32)(page >> K_PAGE_SHIFT)) & PAGE_READ)) {
        klog_fmt("x64: instruction fetch fault at %016llx", (unsigned long long)address);
        dumpState();
        raiseException(K_SIGSEGV, 0);
        return;
    }
    fetchPage = page;
    fetchPtr = p;
}

U8 X64CPU::fetch8() {
    U64 a = rip++;
    if ((a & ~0xFFFULL) != fetchPage) {
        refillFetch(a);
    }
    return fetchPtr[a & 0xFFF];
}

U16 X64CPU::fetch16() {
    U16 lo = fetch8();
    return lo | ((U16)fetch8() << 8);
}

U32 X64CPU::fetch32() {
    if (((rip & 0xFFF) <= 0xFFC) && (rip & ~0xFFFULL) == fetchPage) {
        U32 v;
        memcpy(&v, fetchPtr + (rip & 0xFFF), 4);
        rip += 4;
        return v;
    }
    U32 lo = fetch16();
    return lo | ((U32)fetch16() << 16);
}

U64 X64CPU::fetch64() {
    U64 lo = fetch32();
    return lo | ((U64)fetch32() << 32);
}

void X64CPU::decodeModrm() {
    modrm = fetch8();
    mod = modrm >> 6;
    regField = ((modrm >> 3) & 7) | ((rex & 4) << 1);
    U8 rm = modrm & 7;
    if (mod == 3) {
        isMem = false;
        rmField = rm | ((rex & 1) << 3);
        return;
    }
    isMem = true;
    ripRelative = false;
    U64 addr = 0;
    if (rm == 4) {
        U8 sib = fetch8();
        U8 scale = sib >> 6;
        U8 index = ((sib >> 3) & 7) | ((rex & 2) << 2);
        U8 base = (sib & 7) | ((rex & 1) << 3);
        if (index != 4) {
            addr += r[index] << scale;
        }
        if ((sib & 7) == 5 && mod == 0) {
            addr += (U64)(S64)(S32)fetch32();
        } else {
            addr += r[base];
        }
    } else if (rm == 5 && mod == 0) {
        ripRelative = true;
        addr = (U64)(S64)(S32)fetch32();
    } else {
        addr += r[rm | ((rex & 1) << 3)];
    }
    if (mod == 1) {
        addr += (U64)(S64)(S8)fetch8();
    } else if (mod == 2) {
        addr += (U64)(S64)(S32)fetch32();
    }
    eaPartial = addr;
}

// Must be called after all immediates have been fetched (RIP-relative operands
// are relative to the end of the instruction).
U64 X64CPU::ea() {
    U64 a = eaPartial;
    if (ripRelative) {
        a += rip;
    }
    if (adSize) {
        a = (U32)a;
    }
    if (segPrefix == 4) {
        a += fsBase;
    } else if (segPrefix == 5) {
        a += gsBase;
    }
    return a;
}

U64 X64CPU::getReg(int reg, int bits) {
    switch (bits) {
    case 8:
        if (!rex && reg >= 4 && reg < 8) {
            return (r[reg - 4] >> 8) & 0xFF;
        }
        return r[reg] & 0xFF;
    case 16:
        return r[reg] & 0xFFFF;
    case 32:
        return (U32)r[reg];
    default:
        return r[reg];
    }
}

void X64CPU::setReg(int reg, int bits, U64 value) {
    switch (bits) {
    case 8:
        if (!rex && reg >= 4 && reg < 8) {
            r[reg - 4] = (r[reg - 4] & ~0xFF00ULL) | ((value & 0xFF) << 8);
        } else {
            r[reg] = (r[reg] & ~0xFFULL) | (value & 0xFF);
        }
        break;
    case 16:
        r[reg] = (r[reg] & ~0xFFFFULL) | (value & 0xFFFF);
        break;
    case 32:
        r[reg] = (U32)value;
        break;
    default:
        r[reg] = value;
        break;
    }
}

U64 X64CPU::readRM(int bits) {
    if (isMem) {
        if (lockPrefix) {
            return lockedRead(ea(), bits);
        }
        return readMem(ea(), bits);
    }
    return getReg(rmField, bits);
}

void X64CPU::writeRM(int bits, U64 value) {
    if (isMem) {
        if (lockPrefix) {
            lockedWrite(ea(), bits, value);
        } else {
            writeMem(ea(), bits, value);
        }
    } else {
        setReg(rmField, bits, value);
    }
}

U64 X64CPU::lockedRead(U64 address, int bits) {
    lockPtr = nullptr;
    U32 len = bits / 8;
    if (!(address >> 32) && ((address & 0xFFF) + len) <= K_PAGE_SIZE) {
        U8* p = memory->getRamPtr((U32)address, len, true);
        if (p) {
            lockPtr = p;
            lockOld = x64AtomicLoad(p, bits);
            return lockOld;
        }
    }
    return readMem(address, bits);
}

void X64CPU::lockedWrite(U64 address, int bits, U64 value) {
    if (lockPtr) {
        if (!x64AtomicCas(lockPtr, bits, lockOld, value)) {
            lockRetry = true;
        }
        lockPtr = nullptr;
        return;
    }
    writeMem(address, bits, value);
}

// ---------------------------------------------------------------- ALU helpers

U64 X64CPU::alu(int op, int bits, U64 a, U64 b) {
    U64 m = maskFor(bits);
    U64 res;
    U32 cin;
    a &= m;
    b &= m;
    switch (op) {
    case 0: res = (a + b) & m; setLazy(LF_ADD, bits, a, b, res); break;
    case 1: res = a | b; setLogic(bits, res); break;
    case 2: cin = getCFBit(); res = (a + b + cin) & m; setLazy(LF_ADC, bits, a, b, res); lfCarryIn = cin; break;
    case 3: cin = getCFBit(); res = (a - b - cin) & m; setLazy(LF_SBB, bits, a, b, res); lfCarryIn = cin; break;
    case 4: res = a & b; setLogic(bits, res); break;
    case 5: res = (a - b) & m; setLazy(LF_SUB, bits, a, b, res); break;
    case 6: res = a ^ b; setLogic(bits, res); break;
    default: res = (a - b) & m; setLazy(LF_SUB, bits, a, b, res); break;
    }
    return res;
}

static inline U32 szpFlags(U64 res, int bits) {
    U32 f = 0;
    res &= maskFor(bits);
    if (res == 0) f |= ZF;
    if (res & signFor(bits)) f |= SF;
    f |= parityTable[res & 0xFF];
    return f;
}

U64 X64CPU::shiftOp(int op, int bits, U64 v, U32 count) {
    count &= (bits == 64) ? 0x3F : 0x1F;
    if (count == 0) {
        return v;
    }
    U64 m = maskFor(bits);
    U64 s = signFor(bits);
    v &= m;
    U32 f = getFlags();
    U64 res = v;
    bool cf = (f & CF) != 0;
    bool of = (f & OF) != 0;
    bool setSZP = false;

    switch (op) {
    case 0: { // ROL
        U32 c = count % bits;
        res = c ? (((v << c) | (v >> (bits - c))) & m) : v;
        cf = (res & 1) != 0;
        of = (((res & s) != 0) != cf);
        break;
    }
    case 1: { // ROR
        U32 c = count % bits;
        res = c ? (((v >> c) | (v << (bits - c))) & m) : v;
        cf = (res & s) != 0;
        of = ((res & s) != 0) != ((res & (s >> 1)) != 0);
        break;
    }
    case 2: { // RCL
        U32 c = count % (bits + 1);
        for (U32 i = 0; i < c; i++) {
            bool newCf = (res & s) != 0;
            res = ((res << 1) | (cf ? 1 : 0)) & m;
            cf = newCf;
        }
        of = ((res & s) != 0) != cf;
        break;
    }
    case 3: { // RCR
        U32 c = count % (bits + 1);
        of = ((res & s) != 0) != cf;
        for (U32 i = 0; i < c; i++) {
            bool newCf = (res & 1) != 0;
            res = (res >> 1) | (cf ? s : 0);
            cf = newCf;
        }
        break;
    }
    case 4:
    case 6: // SHL
        res = count < 64 ? ((v << count) & m) : 0;
        cf = count <= (U32)bits ? ((v >> (bits - count)) & 1) != 0 : false;
        of = ((res & s) != 0) != cf;
        setSZP = true;
        break;
    case 5: // SHR
        res = count < 64 ? (v >> count) : 0;
        cf = ((v >> (count - 1)) & 1) != 0;
        of = (v & s) != 0;
        setSZP = true;
        break;
    default: { // SAR
        S64 sv = signExtend(v, bits);
        res = (U64)(sv >> (count > 63 ? 63 : count)) & m;
        cf = ((sv >> (count - 1 > 63 ? 63 : count - 1)) & 1) != 0;
        of = false;
        setSZP = true;
        break;
    }
    }
    f &= ~(CF | OF);
    if (cf) f |= CF;
    if (of) f |= OF;
    if (setSZP) {
        f &= ~(SF | ZF | PF | AF);
        f |= szpFlags(res, bits);
    }
    rflags = f;
    lfOp = LF_NONE;
    return res;
}

U64 X64CPU::shld(int bits, U64 dst, U64 src, U32 count, bool left) {
    count &= (bits == 64) ? 0x3F : 0x1F;
    if (count == 0) {
        return dst;
    }
    U64 m = maskFor(bits);
    U64 s = signFor(bits);
    dst &= m;
    src &= m;
    U64 res;
    bool cf;
    if (bits == 16 && count > 16) {
        // undefined, approximate with a 32 bit shift through both operands
        count &= 15;
        if (count == 0) {
            return dst;
        }
    }
    if (left) {
        res = ((dst << count) | (src >> (bits - count))) & m;
        cf = ((dst >> (bits - count)) & 1) != 0;
    } else {
        res = ((dst >> count) | (src << (bits - count))) & m;
        cf = ((dst >> (count - 1)) & 1) != 0;
    }
    U32 f = getFlags() & ~(FLAGS_ARITH);
    if (cf) f |= CF;
    if (((res ^ dst) & s) != 0) f |= OF;
    f |= szpFlags(res, bits);
    rflags = f;
    lfOp = LF_NONE;
    return res;
}

void X64CPU::group3(int bits) {
    int ext = (modrm >> 3) & 7;
    U64 m = maskFor(bits);
    switch (ext) {
    case 0:
    case 1: { // TEST imm
        U64 imm;
        if (bits == 8) imm = fetch8();
        else if (bits == 16) imm = fetch16();
        else imm = (U64)(S64)(S32)fetch32();
        setLogic(bits, readRM(bits) & imm & m);
        break;
    }
    case 2: // NOT
        writeRM(bits, ~readRM(bits) & m);
        break;
    case 3: { // NEG
        U64 v = readRM(bits) & m;
        U64 res = (0 - v) & m;
        setLazy(LF_NEG, bits, 0, v, res);
        writeRM(bits, res);
        break;
    }
    case 4: { // MUL
        U64 v = readRM(bits) & m;
        U64 lo, hi;
        U32 f = getFlags() & ~(CF | OF);
        switch (bits) {
        case 8: {
            U32 res = (U32)(r[0] & 0xFF) * (U32)v;
            setReg(0, 16, res);
            hi = res >> 8;
            lo = res & 0xFF;
            break;
        }
        case 16: {
            U32 res = (U32)(r[0] & 0xFFFF) * (U32)v;
            setReg(0, 16, res);
            setReg(2, 16, res >> 16);
            hi = res >> 16;
            lo = res & 0xFFFF;
            break;
        }
        case 32: {
            U64 res = (U64)(U32)r[0] * v;
            r[0] = (U32)res;
            r[2] = (U32)(res >> 32);
            hi = res >> 32;
            lo = (U32)res;
            break;
        }
        default:
            x64_umul128(r[0], v, &lo, &hi);
            r[0] = lo;
            r[2] = hi;
            break;
        }
        if (hi) f |= CF | OF;
        f &= ~(SF | ZF | PF);
        f |= szpFlags(lo, bits);
        rflags = f;
        break;
    }
    case 5: { // IMUL
        S64 v = signExtend(readRM(bits), bits);
        U32 f = getFlags() & ~(CF | OF);
        bool overflow;
        U64 lo;
        switch (bits) {
        case 8: {
            S32 res = (S32)(S8)r[0] * (S32)v;
            setReg(0, 16, (U32)res);
            overflow = res != (S8)res;
            lo = (U8)res;
            break;
        }
        case 16: {
            S32 res = (S32)(S16)r[0] * (S32)v;
            setReg(0, 16, (U32)res);
            setReg(2, 16, (U32)res >> 16);
            overflow = res != (S16)res;
            lo = (U16)res;
            break;
        }
        case 32: {
            S64 res = (S64)(S32)r[0] * v;
            r[0] = (U32)res;
            r[2] = (U32)((U64)res >> 32);
            overflow = res != (S32)res;
            lo = (U32)res;
            break;
        }
        default: {
            S64 hi;
            x64_smul128((S64)r[0], v, &lo, &hi);
            r[0] = lo;
            r[2] = (U64)hi;
            overflow = hi != ((S64)lo >> 63);
            break;
        }
        }
        if (overflow) f |= CF | OF;
        f &= ~(SF | ZF | PF);
        f |= szpFlags(lo, bits);
        rflags = f;
        break;
    }
    case 6: { // DIV
        U64 v = readRM(bits) & m;
        if (v == 0) {
            raiseException(K_SIGFPE, 0);
            return;
        }
        switch (bits) {
        case 8: {
            U32 n = (U32)(r[0] & 0xFFFF);
            U32 q = n / (U32)v;
            if (q > 0xFF) { raiseException(K_SIGFPE, 0); return; }
            setReg(0, 16, (q & 0xFF) | ((n % (U32)v) << 8));
            break;
        }
        case 16: {
            U32 n = (U32)(r[0] & 0xFFFF) | ((U32)(r[2] & 0xFFFF) << 16);
            U32 q = n / (U32)v;
            if (q > 0xFFFF) { raiseException(K_SIGFPE, 0); return; }
            setReg(0, 16, q);
            setReg(2, 16, n % (U32)v);
            break;
        }
        case 32: {
            U64 n = (U64)(U32)r[0] | ((U64)(U32)r[2] << 32);
            U64 q = n / v;
            if (q > 0xFFFFFFFFULL) { raiseException(K_SIGFPE, 0); return; }
            r[0] = (U32)q;
            r[2] = (U32)(n % v);
            break;
        }
        default: {
            U64 q, rem;
            if (!x64_udiv128(r[2], r[0], v, &q, &rem)) { raiseException(K_SIGFPE, 0); return; }
            r[0] = q;
            r[2] = rem;
            break;
        }
        }
        break;
    }
    default: { // IDIV
        S64 v = signExtend(readRM(bits), bits);
        if (v == 0) {
            raiseException(K_SIGFPE, 0);
            return;
        }
        switch (bits) {
        case 8: {
            S32 n = (S16)(r[0] & 0xFFFF);
            S32 q = n / (S32)v;
            if (q != (S8)q) { raiseException(K_SIGFPE, 0); return; }
            setReg(0, 16, ((U32)q & 0xFF) | (((U32)(n % (S32)v) & 0xFF) << 8));
            break;
        }
        case 16: {
            S32 n = (S32)((U32)(r[0] & 0xFFFF) | ((U32)(r[2] & 0xFFFF) << 16));
            S32 q = n / (S32)v;
            if (q != (S16)q) { raiseException(K_SIGFPE, 0); return; }
            setReg(0, 16, (U32)q);
            setReg(2, 16, (U32)(n % (S32)v));
            break;
        }
        case 32: {
            S64 n = (S64)((U64)(U32)r[0] | ((U64)(U32)r[2] << 32));
            if (n == INT64_MIN && v == -1) { raiseException(K_SIGFPE, 0); return; }
            S64 q = n / v;
            if (q != (S32)q) { raiseException(K_SIGFPE, 0); return; }
            r[0] = (U32)q;
            r[2] = (U32)(n % v);
            break;
        }
        default: {
            S64 q, rem;
            if (!x64_sdiv128((S64)r[2], r[0], v, &q, &rem)) { raiseException(K_SIGFPE, 0); return; }
            r[0] = (U64)q;
            r[2] = (U64)rem;
            break;
        }
        }
        break;
    }
    }
}

// ---------------------------------------------------------------- misc

void X64CPU::raiseException(int signal, int code) {
    // Signals with a guest handler are delivered via a 64-bit signal frame;
    // otherwise the process dies like it would on Linux.
    if (signal != K_SIGTRAP) {
        rip = instStart;
    }
    x64RunSignal(this, signal);
    throw 3;
}

void X64CPU::unhandled(const char* what, U32 op) {
    klog_fmt("x64: unhandled %s opcode %X", what, op);
    dumpState();
    raiseException(K_SIGILL, 0);
}

void X64CPU::doCpuid() {
    U32 leaf = (U32)r[0];
    U32 sub = (U32)r[1];
    U32 a = 0, b = 0, c = 0, d = 0;
    switch (leaf) {
    case 0:
        a = 0xD;
        b = 0x756e6547; // "GenuineIntel"
        d = 0x49656e69;
        c = 0x6c65746e;
        break;
    case 1:
        a = 0x000306A9; // family 6 model 58 (Ivy Bridge), stepping 9
        b = 0x00010800; // 1 logical processor, clflush 8*8
        // ECX: SSE3 only (no SSSE3/SSE4/AVX/OSXSAVE yet)
        c = (1 << 0);
        // EDX: FPU, TSC, CX8, CMOV, CLFSH, MMX, FXSR, SSE, SSE2
        d = (1 << 0) | (1 << 4) | (1 << 8) | (1 << 15) | (1 << 19) | (1 << 23) | (1 << 24) | (1 << 25) | (1 << 26);
        break;
    case 4:
        // deterministic cache parameters
        switch (sub) {
        case 0: a = 0x121; b = (7 << 22) | 63; c = 63; break;          // L1d 32K 8-way
        case 1: a = 0x122; b = (7 << 22) | 63; c = 63; break;          // L1i 32K 8-way
        case 2: a = 0x143; b = (7 << 22) | 63; c = 511; break;         // L2 256K 8-way
        case 3: a = 0x163; b = (15 << 22) | 63; c = 8191; break;       // L3 8M 16-way
        default: break;
        }
        break;
    case 0x80000000:
        a = 0x80000008;
        break;
    case 0x80000001:
        c = (1 << 0); // LAHF in 64-bit mode
        d = (1 << 11) | (1 << 20) | (1 << 29); // SYSCALL, NX, LM
        break;
    case 0x80000002: a = 0x65786F42; b = 0x6E697764; c = 0x36782065; d = 0x00000034; break; // "Boxedwine x64"
    case 0x80000006:
        c = (256 << 16) | (8 << 12) | 64;
        break;
    case 0x80000008:
        a = 0x3027; // 39 bit physical, 48 bit virtual
        break;
    default:
        break;
    }
    r[0] = a;
    r[3] = b;
    r[1] = c;
    r[2] = d;
}

void X64CPU::doSyscall() {
    U64 next = rip;
    r[1] = next;            // RCX = return address
    r[11] = getFlags();     // R11 = RFLAGS
    ksyscall64(this);
    invalidateFetchCache();
}

void X64CPU::doString(U8 op) {
    int bits;
    switch (op) {
    case 0xA4: case 0xA6: case 0xAA: case 0xAC: case 0xAE:
        bits = 8;
        break;
    default:
        bits = opBits();
        break;
    }
    S64 delta = (getFlags() & DF) ? -(bits / 8) : (bits / 8);
    U64 amask = adSize ? 0xFFFFFFFFULL : ~0ULL;
    U64 segBase = segPrefix == 4 ? fsBase : (segPrefix == 5 ? gsBase : 0);
    bool rep = repPrefix != 0;
    U64 count = rep ? (r[1] & amask) : 1;

    // fast paths for the most common forward byte/qword copies and fills
    if (rep && delta > 0 && !segBase && (op == 0xAA || op == 0xAB) && count) {
        U64 dst = r[7] & amask;
        U64 total = count * (bits / 8);
        if (!((dst + total) >> 32)) {
            if (bits == 8) {
                memory->memset((U32)dst, (U8)r[0], (U32)total);
            } else {
                U64 v = getReg(0, bits);
                for (U64 i = 0; i < count; i++) {
                    writeMem(dst + i * (bits / 8), bits, v);
                }
            }
            r[7] = (r[7] & ~amask) | ((dst + total) & amask);
            r[1] &= ~amask;
            return;
        }
    }
    if (rep && delta > 0 && !segBase && (op == 0xA4 || op == 0xA5) && count) {
        U64 src = r[6] & amask;
        U64 dst = r[7] & amask;
        U64 total = count * (bits / 8);
        // only if regions don't overlap in a way that breaks forward copying
        if (!((dst + total) >> 32) && !((src + total) >> 32) && (dst + total <= src || src + total <= dst)) {
            memory->memcpy((U32)dst, (U32)src, (U32)total);
            r[6] = (r[6] & ~amask) | ((src + total) & amask);
            r[7] = (r[7] & ~amask) | ((dst + total) & amask);
            r[1] &= ~amask;
            return;
        }
    }

    while (count) {
        U64 si = r[6] & amask;
        U64 di = r[7] & amask;
        bool stop = false;
        switch (op) {
        case 0xA4: case 0xA5: // MOVS
            writeMem(di, bits, readMem(segBase + si, bits));
            r[6] = (r[6] & ~amask) | ((si + delta) & amask);
            r[7] = (r[7] & ~amask) | ((di + delta) & amask);
            break;
        case 0xA6: case 0xA7: { // CMPS
            U64 a = readMem(segBase + si, bits);
            U64 b = readMem(di, bits);
            alu(7, bits, a, b);
            r[6] = (r[6] & ~amask) | ((si + delta) & amask);
            r[7] = (r[7] & ~amask) | ((di + delta) & amask);
            if (rep) {
                bool zf = cond(4);
                stop = (repPrefix == 0xF3) ? !zf : zf;
            }
            break;
        }
        case 0xAA: case 0xAB: // STOS
            writeMem(di, bits, getReg(0, bits));
            r[7] = (r[7] & ~amask) | ((di + delta) & amask);
            break;
        case 0xAC: case 0xAD: // LODS
            setReg(0, bits, readMem(segBase + si, bits));
            r[6] = (r[6] & ~amask) | ((si + delta) & amask);
            break;
        default: { // SCAS
            U64 b = readMem(di, bits);
            alu(7, bits, getReg(0, bits), b);
            r[7] = (r[7] & ~amask) | ((di + delta) & amask);
            if (rep) {
                bool zf = cond(4);
                stop = (repPrefix == 0xF3) ? !zf : zf;
            }
            break;
        }
        }
        count--;
        if (rep) {
            r[1] = (r[1] & ~amask) | (count & amask);
        }
        if (stop) {
            break;
        }
    }
}

// ---------------------------------------------------------------- main loop

void X64CPU::run() {
    if (thread->terminating) {
        return;
    }
    if (thread->pendingSignals) {
        U64 mask = thread->sigMask;
        U64 todo = (thread->pendingSignals | thread->process->pendingSignals) & ~mask;
        if (todo) {
            for (U32 i = 0; i < 64; i++) {
                if (todo & (1ULL << i)) {
                    {
                        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(thread->pendingSignalsMutex);
                        thread->pendingSignals &= ~(1ULL << i);
                    }
                    x64RunSignal(this, i + 1);
                    return;
                }
            }
        }
    }
    for (U32 i = 0; i < 100000; i++) {
        step();
        if (lockRetry) {
            lockRetry = false;
            rip = instStart;
        }
        if (syscallInvalidatedFetch) {
            syscallInvalidatedFetch = false;
            if (thread->terminating || thread->pendingSignals || yield || thread->cpu != this) {
                return;
            }
        }
    }
}

void X64CPU::step() {
    instStart = rip;
    rex = 0;
    opSize = false;
    adSize = false;
    repPrefix = 0;
    segPrefix = 0;
    lockPrefix = false;
    lockPtr = nullptr;

    U8 op;
    while (true) {
        op = fetch8();
        switch (op) {
        case 0x66: opSize = true; continue;
        case 0x67: adSize = true; continue;
        case 0xF2: case 0xF3: repPrefix = op; continue;
        case 0xF0: lockPrefix = true; continue;
        case 0x26: case 0x2E: case 0x36: case 0x3E: continue;
        case 0x64: segPrefix = 4; continue;
        case 0x65: segPrefix = 5; continue;
        default: break;
        }
        break;
    }
    if ((op & 0xF0) == 0x40) {
        rex = op;
        op = fetch8();
    }

    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3;
        switch (op & 7) {
        case 0: {
            decodeModrm();
            U64 res = alu(aop, 8, readRM(8), getReg(regField, 8));
            if (aop != 7) writeRM(8, res);
            break;
        }
        case 1: {
            int bits = opBits();
            decodeModrm();
            U64 res = alu(aop, bits, readRM(bits), getReg(regField, bits));
            if (aop != 7) writeRM(bits, res);
            break;
        }
        case 2: {
            decodeModrm();
            U64 res = alu(aop, 8, getReg(regField, 8), readRM(8));
            if (aop != 7) setReg(regField, 8, res);
            break;
        }
        case 3: {
            int bits = opBits();
            decodeModrm();
            U64 res = alu(aop, bits, getReg(regField, bits), readRM(bits));
            if (aop != 7) setReg(regField, bits, res);
            break;
        }
        case 4: {
            U64 imm = fetch8();
            U64 res = alu(aop, 8, r[0], imm);
            if (aop != 7) setReg(0, 8, res);
            break;
        }
        case 5: {
            int bits = opBits();
            U64 imm = bits == 16 ? fetch16() : (U64)(S64)(S32)fetch32();
            U64 res = alu(aop, bits, r[0], imm);
            if (aop != 7) setReg(0, bits, res);
            break;
        }
        }
        return;
    }

    switch (op) {
    case 0x0F:
        step0F();
        return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        int reg = (op & 7) | ((rex & 1) << 3);
        if (opSize) {
            U64 sp = r[RSP_INDEX] - 2;
            write16(sp, (U16)r[reg]);
            r[RSP_INDEX] = sp;
        } else {
            push64(r[reg]);
        }
        return;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int reg = (op & 7) | ((rex & 1) << 3);
        if (opSize) {
            U16 v = read16(r[RSP_INDEX]);
            r[RSP_INDEX] += 2;
            setReg(reg, 16, v);
        } else {
            r[reg] = pop64();
        }
        return;
    }
    case 0x63: { // MOVSXD
        decodeModrm();
        if (rex & 8) {
            setReg(regField, 64, (U64)(S64)(S32)readRM(32));
        } else {
            setReg(regField, opBits(), readRM(opBits()));
        }
        return;
    }
    case 0x68: {
        if (opSize) {
            U16 v = fetch16();
            U64 sp = r[RSP_INDEX] - 2;
            write16(sp, v);
            r[RSP_INDEX] = sp;
        } else {
            push64((U64)(S64)(S32)fetch32());
        }
        return;
    }
    case 0x6A:
        push64((U64)(S64)(S8)fetch8());
        return;
    case 0x69:
    case 0x6B: {
        int bits = opBits();
        decodeModrm();
        S64 imm;
        if (op == 0x6B) imm = (S8)fetch8();
        else if (bits == 16) imm = (S16)fetch16();
        else imm = (S32)fetch32();
        S64 a = signExtend(readRM(bits), bits);
        U64 lo;
        bool overflow;
        if (bits == 64) {
            S64 hi;
            x64_smul128(a, imm, &lo, &hi);
            overflow = hi != ((S64)lo >> 63);
        } else {
            S64 res = a * imm;
            lo = (U64)res & maskFor(bits);
            overflow = res != signExtend(lo, bits);
        }
        U32 f = getFlags() & ~(CF | OF | SF | ZF | PF);
        if (overflow) f |= CF | OF;
        f |= szpFlags(lo, bits);
        rflags = f;
        setReg(regField, bits, lo);
        return;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        S8 rel = (S8)fetch8();
        if (cond(op & 0xF)) {
            rip += (S64)rel;
        }
        return;
    }
    case 0x80:
    case 0x82: {
        decodeModrm();
        U64 imm = fetch8();
        int aop = (modrm >> 3) & 7;
        U64 res = alu(aop, 8, readRM(8), imm);
        if (aop != 7) writeRM(8, res);
        return;
    }
    case 0x81:
    case 0x83: {
        int bits = opBits();
        decodeModrm();
        U64 imm;
        if (op == 0x83) imm = (U64)(S64)(S8)fetch8();
        else if (bits == 16) imm = fetch16();
        else imm = (U64)(S64)(S32)fetch32();
        int aop = (modrm >> 3) & 7;
        U64 res = alu(aop, bits, readRM(bits), imm);
        if (aop != 7) writeRM(bits, res);
        return;
    }
    case 0x84:
        decodeModrm();
        setLogic(8, readRM(8) & getReg(regField, 8));
        return;
    case 0x85: {
        int bits = opBits();
        decodeModrm();
        setLogic(bits, readRM(bits) & getReg(regField, bits));
        return;
    }
    case 0x86:
    case 0x87: {
        int bits = op == 0x86 ? 8 : opBits();
        decodeModrm();
        lockPrefix = isMem;
        U64 a = readRM(bits);
        writeRM(bits, getReg(regField, bits));
        if (lockRetry) return;
        setReg(regField, bits, a);
        return;
    }
    case 0x88:
        decodeModrm();
        writeRM(8, getReg(regField, 8));
        return;
    case 0x89: {
        int bits = opBits();
        decodeModrm();
        writeRM(bits, getReg(regField, bits));
        return;
    }
    case 0x8A:
        decodeModrm();
        setReg(regField, 8, readRM(8));
        return;
    case 0x8B: {
        int bits = opBits();
        decodeModrm();
        setReg(regField, bits, readRM(bits));
        return;
    }
    case 0x8C: { // MOV r/m, Sreg
        decodeModrm();
        static const U16 selectors[8] = { 0, 0x33, 0x2B, 0, 0, 0, 0, 0 };
        U16 v = selectors[(modrm >> 3) & 7];
        if (isMem) writeRM(16, v);
        else setReg(rmField, opBits(), v);
        return;
    }
    case 0x8D: { // LEA
        int bits = opBits();
        decodeModrm();
        U64 a = eaPartial + (ripRelative ? rip : 0);
        if (adSize) a = (U32)a;
        setReg(regField, bits, a);
        return;
    }
    case 0x8E: // MOV Sreg, r/m (ignored in 64-bit flat mode)
        decodeModrm();
        readRM(16);
        return;
    case 0x8F: { // POP r/m
        U64 v = pop64();
        decodeModrm();
        writeRM(64, v);
        return;
    }
    case 0x90:
        if (rex & 1) {
            U64 t = r[8];
            if (opBits() == 64) { r[8] = r[0]; r[0] = t; }
            else { int bits = opBits(); U64 a = getReg(0, bits); setReg(0, bits, getReg(8, bits)); setReg(8, bits, a); }
        }
        return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        int reg = (op & 7) | ((rex & 1) << 3);
        int bits = opBits();
        U64 a = getReg(0, bits);
        setReg(0, bits, getReg(reg, bits));
        setReg(reg, bits, a);
        return;
    }
    case 0x98:
        if (rex & 8) r[0] = (U64)(S64)(S32)r[0];
        else if (opSize) setReg(0, 16, (U16)(S16)(S8)r[0]);
        else r[0] = (U32)(S32)(S16)r[0];
        return;
    case 0x99:
        if (rex & 8) r[2] = (U64)((S64)r[0] >> 63);
        else if (opSize) setReg(2, 16, (U16)((S16)r[0] >> 15));
        else r[2] = (U32)((S32)r[0] >> 31);
        return;
    case 0x9B: // FWAIT
        return;
    case 0x9C:
        if (opSize) {
            U64 sp = r[RSP_INDEX] - 2;
            write16(sp, (U16)getFlags());
            r[RSP_INDEX] = sp;
        } else {
            push64(getFlags());
        }
        return;
    case 0x9D:
        if (opSize) {
            U16 v = read16(r[RSP_INDEX]);
            r[RSP_INDEX] += 2;
            setFlagsValue((getFlags() & 0xFFFF0000) | v);
        } else {
            setFlagsValue((U32)pop64());
        }
        return;
    case 0x9E: { // SAHF
        U32 f = getFlags() & ~(SF | ZF | AF | PF | CF);
        f |= (r[0] >> 8) & (SF | ZF | AF | PF | CF);
        rflags = f;
        return;
    }
    case 0x9F: // LAHF
        r[0] = (r[0] & ~0xFF00ULL) | (((getFlags() & 0xD5) | 2) << 8);
        return;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        U64 address = adSize ? fetch32() : fetch64();
        if (segPrefix == 4) address += fsBase;
        else if (segPrefix == 5) address += gsBase;
        int bits = (op & 1) ? opBits() : 8;
        if (op < 0xA2) setReg(0, bits, readMem(address, bits));
        else writeMem(address, bits, getReg(0, bits));
        return;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        doString(op);
        return;
    case 0xA8:
        setLogic(8, r[0] & fetch8());
        return;
    case 0xA9: {
        int bits = opBits();
        U64 imm = bits == 16 ? fetch16() : (U64)(S64)(S32)fetch32();
        setLogic(bits, getReg(0, bits) & imm);
        return;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setReg((op & 7) | ((rex & 1) << 3), 8, fetch8());
        return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int reg = (op & 7) | ((rex & 1) << 3);
        if (rex & 8) r[reg] = fetch64();
        else if (opSize) setReg(reg, 16, fetch16());
        else r[reg] = fetch32();
        return;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int bits = (op & 1) ? opBits() : 8;
        decodeModrm();
        U32 count;
        if (op <= 0xC1) count = fetch8();
        else if (op <= 0xD1) count = 1;
        else count = (U32)(r[1] & 0xFF);
        U64 v = readRM(bits);
        U64 res = shiftOp((modrm >> 3) & 7, bits, v, count);
        writeRM(bits, res);
        return;
    }
    case 0xC2: {
        U16 n = fetch16();
        rip = pop64();
        r[RSP_INDEX] += n;
        return;
    }
    case 0xC3:
        rip = pop64();
        return;
    case 0xC6:
        decodeModrm();
        if (modrm == 0xF8) { fetch8(); return; } // XABORT
        {
            U64 imm = fetch8();
            writeRM(8, imm);
        }
        return;
    case 0xC7: {
        int bits = opBits();
        decodeModrm();
        if (modrm == 0xF8) { fetch32(); return; } // XBEGIN: never in a transaction
        U64 imm = bits == 16 ? fetch16() : (U64)(S64)(S32)fetch32();
        writeRM(bits, imm);
        return;
    }
    case 0xC8: { // ENTER
        U16 size = fetch16();
        U8 level = fetch8() & 0x1F;
        push64(r[5]);
        U64 frame = r[RSP_INDEX];
        for (U32 i = 1; i < level; i++) {
            r[5] -= 8;
            push64(read64(r[5]));
        }
        if (level) push64(frame);
        r[5] = frame;
        r[RSP_INDEX] -= size;
        return;
    }
    case 0xC9: // LEAVE
        r[RSP_INDEX] = r[5];
        r[5] = pop64();
        return;
    case 0xCC:
        raiseException(K_SIGTRAP, 0);
        return;
    case 0xCD:
        fetch8();
        raiseException(K_SIGSEGV, 0);
        return;
    case 0xD7: { // XLAT
        U64 address = (adSize ? (U32)r[3] : r[3]) + (r[0] & 0xFF);
        if (segPrefix == 4) address += fsBase;
        else if (segPrefix == 5) address += gsBase;
        setReg(0, 8, read8(address));
        return;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        stepFPU(op);
        return;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        S8 rel = (S8)fetch8();
        U64 amask = adSize ? 0xFFFFFFFFULL : ~0ULL;
        bool jump;
        if (op == 0xE3) {
            jump = (r[1] & amask) == 0;
        } else {
            U64 c = ((r[1] & amask) - 1) & amask;
            r[1] = (r[1] & ~amask) | c;
            jump = c != 0;
            if (op == 0xE0) jump = jump && !cond(4);
            else if (op == 0xE1) jump = jump && cond(4);
        }
        if (jump) rip += (S64)rel;
        return;
    }
    case 0xE8: {
        S32 rel = (S32)fetch32();
        push64(rip);
        rip += (S64)rel;
        return;
    }
    case 0xE9: {
        S32 rel = (S32)fetch32();
        rip += (S64)rel;
        return;
    }
    case 0xEB: {
        S8 rel = (S8)fetch8();
        rip += (S64)rel;
        return;
    }
    case 0xF4:
        raiseException(K_SIGSEGV, 0);
        return;
    case 0xF5:
        rflags = getFlags() ^ CF;
        return;
    case 0xF6:
        decodeModrm();
        group3(8);
        return;
    case 0xF7:
        decodeModrm();
        group3(opBits());
        return;
    case 0xF8:
        rflags = getFlags() & ~CF;
        return;
    case 0xF9:
        rflags = getFlags() | CF;
        return;
    case 0xFC:
        rflags &= ~DF;
        return;
    case 0xFD:
        rflags |= DF;
        return;
    case 0xFE:
    case 0xFF: {
        int bits = op == 0xFE ? 8 : opBits();
        decodeModrm();
        int ext = (modrm >> 3) & 7;
        switch (ext) {
        case 0:
        case 1: {
            U32 cin = getCFBit();
            U64 v = readRM(bits);
            U64 res = (ext == 0 ? v + 1 : v - 1) & maskFor(bits);
            setLazy(ext == 0 ? LF_INC : LF_DEC, bits, v, 1, res);
            lfCarryIn = cin;
            writeRM(bits, res);
            return;
        }
        case 2: {
            U64 target = readRM(64);
            push64(rip);
            rip = target;
            return;
        }
        case 4:
            rip = readRM(64);
            return;
        case 6:
            push64(readRM(64));
            return;
        default:
            if (op == 0xFF) unhandled("FF", ext);
            unhandled("FE", ext);
            return;
        }
    }
    default:
        unhandled("one byte", op);
        return;
    }
}

void X64CPU::step0F() {
    U8 op = fetch8();
    switch (op) {
    case 0x01: {
        U8 m = fetch8();
        if (m == 0xD0) { // XGETBV
            r[0] = 3;
            r[2] = 0;
            return;
        }
        if (m == 0xF9) { // RDTSCP
            U64 t = KSystem::getSystemTimeAsMicroSeconds() * 1000;
            r[0] = (U32)t;
            r[2] = (U32)(t >> 32);
            r[1] = 0;
            return;
        }
        unhandled("0F 01", m);
        return;
    }
    case 0x05:
        doSyscall();
        return;
    case 0x0B:
        raiseException(K_SIGILL, 0);
        return;
    case 0x0D: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        decodeModrm(); // prefetch / hint nops / endbr64
        return;
    case 0x31: { // RDTSC
        U64 t = KSystem::getSystemTimeAsMicroSeconds() * 1000;
        r[0] = (U32)t;
        r[2] = (U32)(t >> 32);
        return;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        int bits = opBits();
        decodeModrm();
        U64 v = readRM(bits);
        if (cond(op & 0xF)) {
            setReg(regField, bits, v);
        } else if (bits == 32) {
            r[regField] = (U32)r[regField];
        }
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        S32 rel = (S32)fetch32();
        if (cond(op & 0xF)) {
            rip += (S64)rel;
        }
        return;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        decodeModrm();
        writeRM(8, cond(op & 0xF) ? 1 : 0);
        return;
    case 0xA2:
        doCpuid();
        return;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB:
    case 0xBA: {
        int bits = opBits();
        decodeModrm();
        int kind;
        U64 bitOffset;
        U64 address = 0;
        if (op == 0xBA) {
            kind = (modrm >> 3) & 7;
            if (kind < 4) {
                unhandled("0F BA", kind);
                return;
            }
            kind -= 4;
            bitOffset = fetch8() & (bits - 1);
            if (isMem) address = ea();
        } else {
            kind = op == 0xA3 ? 0 : (op == 0xAB ? 1 : (op == 0xB3 ? 2 : 3));
            S64 off = signExtend(getReg(regField, bits), bits);
            if (isMem) {
                address = ea() + (U64)((off >> (bits == 64 ? 6 : (bits == 32 ? 5 : 4))) * (bits / 8));
                bitOffset = (U64)off & (bits - 1);
            } else {
                bitOffset = (U64)off & (bits - 1);
            }
        }
        U64 v = isMem ? (lockPrefix && kind != 0 ? lockedRead(address, bits) : readMem(address, bits)) : getReg(rmField, bits);
        U64 bit = 1ULL << bitOffset;
        U32 f = getFlags() & ~CF;
        if (v & bit) f |= CF;
        rflags = f;
        if (kind == 0) return;
        if (kind == 1) v |= bit;
        else if (kind == 2) v &= ~bit;
        else v ^= bit;
        if (isMem) {
            if (lockPrefix) lockedWrite(address, bits, v);
            else writeMem(address, bits, v);
        } else {
            setReg(rmField, bits, v);
        }
        return;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
        int bits = opBits();
        decodeModrm();
        U32 count = (op == 0xA4 || op == 0xAC) ? fetch8() : (U32)(r[1] & 0xFF);
        U64 res = shld(bits, readRM(bits), getReg(regField, bits), count, op < 0xA8);
        writeRM(bits, res);
        return;
    }
    case 0xAE: {
        decodeModrm();
        int ext = (modrm >> 3) & 7;
        if (!isMem) {
            if (repPrefix == 0xF3) { // RD/WR FS/GS BASE
                int bits = (rex & 8) ? 64 : 32;
                switch (ext) {
                case 0: setReg(rmField, bits, fsBase); return;
                case 1: setReg(rmField, bits, gsBase); return;
                case 2: fsBase = getReg(rmField, bits); return;
                case 3: gsBase = getReg(rmField, bits); return;
                }
            }
            return; // LFENCE / MFENCE / SFENCE
        }
        U64 address = ea();
        switch (ext) {
        case 0: // FXSAVE
        case 4: { // XSAVE (legacy region only)
            write16(address, fpu.CW());
            write16(address + 2, fpu.SW());
            write8(address + 4, 0);
            write32(address + 24, mxcsr);
            write32(address + 28, 0xFFFF);
            for (int i = 0; i < 16; i++) {
                writeXmm(address + 160 + i * 16, x[i]);
            }
            return;
        }
        case 1: // FXRSTOR
        case 5: { // XRSTOR
            fpu.SetCW(read16(address));
            mxcsr = read32(address + 24);
            for (int i = 0; i < 16; i++) {
                readXmm(address + 160 + i * 16, x[i]);
            }
            return;
        }
        case 2:
            mxcsr = read32(address);
            return;
        case 3:
            write32(address, mxcsr);
            return;
        default: // CLFLUSH etc
            return;
        }
    }
    case 0xAF: {
        int bits = opBits();
        decodeModrm();
        S64 a = signExtend(getReg(regField, bits), bits);
        S64 b = signExtend(readRM(bits), bits);
        U64 lo;
        bool overflow;
        if (bits == 64) {
            S64 hi;
            x64_smul128(a, b, &lo, &hi);
            overflow = hi != ((S64)lo >> 63);
        } else {
            S64 res = a * b;
            lo = (U64)res & maskFor(bits);
            overflow = res != signExtend(lo, bits);
        }
        U32 f = getFlags() & ~(CF | OF | SF | ZF | PF);
        if (overflow) f |= CF | OF;
        f |= szpFlags(lo, bits);
        rflags = f;
        setReg(regField, bits, lo);
        return;
    }
    case 0xB0:
    case 0xB1: {
        int bits = op == 0xB0 ? 8 : opBits();
        decodeModrm();
        U64 dst = readRM(bits);
        U64 acc = getReg(0, bits);
        alu(7, bits, acc, dst);
        if ((acc & maskFor(bits)) == (dst & maskFor(bits))) {
            writeRM(bits, getReg(regField, bits));
        } else {
            setReg(0, bits, dst);
        }
        return;
    }
    case 0xB6:
        decodeModrm();
        setReg(regField, opBits(), readRM(8));
        return;
    case 0xB7:
        decodeModrm();
        setReg(regField, opBits(), readRM(16));
        return;
    case 0xBE:
        decodeModrm();
        setReg(regField, opBits(), (U64)(S64)(S8)readRM(8));
        return;
    case 0xBF:
        decodeModrm();
        setReg(regField, opBits(), (U64)(S64)(S16)readRM(16));
        return;
    case 0xBC:
    case 0xBD: {
        int bits = opBits();
        decodeModrm();
        U64 v = readRM(bits) & maskFor(bits);
        U32 f = getFlags() & ~(ZF | CF);
        if (repPrefix == 0xF3 && op == 0xBD) { // LZCNT is not advertised, behave like BSR
        }
        if (v == 0) {
            f |= ZF;
        } else {
            U32 index = 0;
            if (op == 0xBC) {
                while (!((v >> index) & 1)) index++;
            } else {
                index = bits - 1;
                while (!((v >> index) & 1)) index--;
            }
            setReg(regField, bits, index);
        }
        rflags = f;
        return;
    }
    case 0xC0:
    case 0xC1: {
        int bits = op == 0xC0 ? 8 : opBits();
        decodeModrm();
        U64 dst = readRM(bits);
        U64 src = getReg(regField, bits);
        U64 res = alu(0, bits, dst, src);
        writeRM(bits, res);
        if (lockRetry) return;
        setReg(regField, bits, dst);
        return;
    }
    case 0xC3: { // MOVNTI
        int bits = opBits();
        decodeModrm();
        writeRM(bits, getReg(regField, bits));
        return;
    }
    case 0xC7: {
        decodeModrm();
        int ext = (modrm >> 3) & 7;
        if (ext == 1 && isMem) {
            U64 address = ea();
            U32 f = getFlags() & ~ZF;
            if (rex & 8) { // CMPXCHG16B
                U8* p = (!(address >> 32) && (address & 0xFFF) <= 0xFF0) ? memory->getRamPtr((U32)address, 16, true) : nullptr;
                if (p) {
                    U64 expected[2] = { r[0], r[2] };
                    if (x64AtomicCas128(p, expected, r[3], r[1])) {
                        f |= ZF;
                    } else {
                        r[0] = expected[0];
                        r[2] = expected[1];
                    }
                } else {
                    U64 lo = read64(address), hi = read64(address + 8);
                    if (lo == r[0] && hi == r[2]) {
                        write64(address, r[3]);
                        write64(address + 8, r[1]);
                        f |= ZF;
                    } else {
                        r[0] = lo;
                        r[2] = hi;
                    }
                }
            } else { // CMPXCHG8B
                U64 cmp = (U64)(U32)r[0] | ((U64)(U32)r[2] << 32);
                U64 desired = (U64)(U32)r[3] | ((U64)(U32)r[1] << 32);
                U8* p = (!(address >> 32) && (address & 0xFFF) <= 0xFF8) ? memory->getRamPtr((U32)address, 8, true) : nullptr;
                U64 v;
                bool ok;
                if (p) {
                    v = x64AtomicLoad(p, 64);
                    ok = v == cmp && x64AtomicCas(p, 64, cmp, desired);
                    if (v == cmp && !ok) {
                        lockRetry = true;
                        return;
                    }
                } else {
                    v = read64(address);
                    ok = v == cmp;
                    if (ok) write64(address, desired);
                }
                if (ok) {
                    f |= ZF;
                } else {
                    r[0] = (U32)v;
                    r[2] = (U32)(v >> 32);
                }
            }
            rflags = f;
            return;
        }
        unhandled("0F C7", ext);
        return;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        int reg = (op & 7) | ((rex & 1) << 3);
        if (rex & 8) {
            U64 v = r[reg], res = 0;
            for (int i = 0; i < 8; i++) res |= ((v >> (i * 8)) & 0xFF) << ((7 - i) * 8);
            r[reg] = res;
        } else {
            U32 v = (U32)r[reg];
            r[reg] = (U32)((v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24));
        }
        return;
    }
    case 0x38:
        step0F38();
        return;
    case 0x3A:
        step0F3A();
        return;
    default:
        if (stepSSE(op)) {
            return;
        }
        unhandled("0F", op);
        return;
    }
}
