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
#include <math.h>

// SSE / SSE2 / SSE3 subset for the x86-64 interpreter (the baseline every
// x86-64 program may assume). Prefix selects the variant:
// 0 = none (ps), 1 = 66 (pd / integer), 2 = F3 (ss), 3 = F2 (sd).

static inline S8 satS8(S32 v) { return (S8)(v < -128 ? -128 : (v > 127 ? 127 : v)); }
static inline U8 satU8(S32 v) { return (U8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
static inline S16 satS16(S32 v) { return (S16)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }
static inline U16 satU16(S32 v) { return (U16)(v < 0 ? 0 : (v > 65535 ? 65535 : v)); }

static double roundMode(double v, U32 mxcsr, bool truncate) {
    if (truncate) {
        return trunc(v);
    }
    switch ((mxcsr >> 13) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static S64 cvtToInt(double v, int bits, U32 mxcsr, bool truncate) {
    if (v != v) {
        return bits == 64 ? (S64)0x8000000000000000ULL : (S64)(S32)0x80000000;
    }
    double r = roundMode(v, mxcsr, truncate);
    if (bits == 64) {
        if (r >= 9223372036854775808.0 || r < -9223372036854775808.0) return (S64)0x8000000000000000ULL;
        return (S64)r;
    }
    if (r >= 2147483648.0 || r < -2147483648.0) return (S32)0x80000000;
    return (S32)r;
}

template <typename T>
static inline T fop(int op, T a, T b) {
    switch (op) {
    case 0x58: return a + b;
    case 0x59: return a * b;
    case 0x5C: return a - b;
    case 0x5D: return (a < b) ? a : b; // MIN: returns src if either is NaN
    case 0x5E: return a / b;
    default: return (a > b) ? a : b;   // MAX
    }
}

template <typename T>
static inline bool fcmp(int pred, T a, T b) {
    bool unord = (a != a) || (b != b);
    switch (pred & 7) {
    case 0: return !unord && a == b;
    case 1: return !unord && a < b;
    case 2: return !unord && a <= b;
    case 3: return unord;
    case 4: return unord || a != b;
    case 5: return unord || !(a < b);
    case 6: return unord || !(a <= b);
    default: return !unord;
    }
}

bool X64CPU::stepSSE(U8 op) {
    int pfx = repPrefix == 0xF3 ? 2 : (repPrefix == 0xF2 ? 3 : (opSize ? 1 : 0));
    X64Xmm s;
    X64Xmm res;

#define LOAD128(v) do { if (isMem) readXmm(ea(), v); else v = x[rmField]; } while (0)
#define LOAD64() (isMem ? read64(ea()) : x[rmField].q[0])
#define LOAD32() (isMem ? read32(ea()) : x[rmField].d[0])
#define D x[regField]

    switch (op) {
    case 0x10:
        decodeModrm();
        if (pfx < 2) {
            LOAD128(s);
            D = s;
        } else if (pfx == 2) {
            if (isMem) { D.q[0] = read32(ea()); D.q[1] = 0; }
            else D.d[0] = x[rmField].d[0];
        } else {
            if (isMem) { D.q[0] = read64(ea()); D.q[1] = 0; }
            else D.q[0] = x[rmField].q[0];
        }
        return true;
    case 0x11:
        decodeModrm();
        if (pfx < 2) {
            if (isMem) writeXmm(ea(), D); else x[rmField] = D;
        } else if (pfx == 2) {
            if (isMem) write32(ea(), D.d[0]); else x[rmField].d[0] = D.d[0];
        } else {
            if (isMem) write64(ea(), D.q[0]); else x[rmField].q[0] = D.q[0];
        }
        return true;
    case 0x12:
        decodeModrm();
        if (pfx == 0 || pfx == 1) {
            if (isMem) D.q[0] = read64(ea());
            else D.q[0] = x[rmField].q[1]; // MOVHLPS
        } else if (pfx == 3) { // MOVDDUP
            U64 v = LOAD64();
            D.q[0] = v;
            D.q[1] = v;
        } else { // MOVSLDUP
            LOAD128(s);
            D.d[0] = s.d[0]; D.d[1] = s.d[0]; D.d[2] = s.d[2]; D.d[3] = s.d[2];
        }
        return true;
    case 0x13:
        decodeModrm();
        if (isMem) write64(ea(), D.q[0]);
        return true;
    case 0x14:
        decodeModrm();
        LOAD128(s);
        if (pfx == 0) { res.d[0] = D.d[0]; res.d[1] = s.d[0]; res.d[2] = D.d[1]; res.d[3] = s.d[1]; }
        else { res.q[0] = D.q[0]; res.q[1] = s.q[0]; }
        D = res;
        return true;
    case 0x15:
        decodeModrm();
        LOAD128(s);
        if (pfx == 0) { res.d[0] = D.d[2]; res.d[1] = s.d[2]; res.d[2] = D.d[3]; res.d[3] = s.d[3]; }
        else { res.q[0] = D.q[1]; res.q[1] = s.q[1]; }
        D = res;
        return true;
    case 0x16:
        decodeModrm();
        if (pfx == 2) { // MOVSHDUP
            LOAD128(s);
            D.d[0] = s.d[1]; D.d[1] = s.d[1]; D.d[2] = s.d[3]; D.d[3] = s.d[3];
        } else {
            D.q[1] = LOAD64(); // MOVHPS/MOVHPD/MOVLHPS
        }
        return true;
    case 0x17:
        decodeModrm();
        if (isMem) write64(ea(), D.q[1]);
        return true;
    case 0x28:
        decodeModrm();
        LOAD128(s);
        D = s;
        return true;
    case 0x29:
    case 0x2B:
        decodeModrm();
        if (isMem) writeXmm(ea(), D); else x[rmField] = D;
        return true;
    case 0x2A: {
        decodeModrm();
        if (pfx < 2) return false; // MMX forms
        S64 v = (rex & 8) ? (S64)readRM(64) : (S64)(S32)readRM(32);
        if (pfx == 2) D.f[0] = (float)v;
        else D.fd[0] = (double)v;
        return true;
    }
    case 0x2C:
    case 0x2D: {
        decodeModrm();
        if (pfx < 2) return false;
        double v;
        if (pfx == 2) {
            U32 bitsF = LOAD32();
            float f;
            memcpy(&f, &bitsF, 4);
            v = f;
        } else {
            U64 bitsD = LOAD64();
            memcpy(&v, &bitsD, 8);
        }
        int bits = (rex & 8) ? 64 : 32;
        setReg(regField, bits, (U64)cvtToInt(v, bits, mxcsr, op == 0x2C));
        return true;
    }
    case 0x2E:
    case 0x2F: {
        decodeModrm();
        double a, b;
        if (pfx == 0) {
            U32 bv = LOAD32();
            float fb;
            memcpy(&fb, &bv, 4);
            a = D.f[0];
            b = fb;
        } else {
            U64 bv = LOAD64();
            memcpy(&b, &bv, 8);
            a = D.fd[0];
        }
        U32 f = getFlags() & ~(CF | PF | AF | ZF | SF | OF);
        if (a != a || b != b) f |= ZF | PF | CF;
        else if (a < b) f |= CF;
        else if (a == b) f |= ZF;
        rflags = f;
        return true;
    }
    case 0x50: {
        decodeModrm();
        X64Xmm& v = x[rmField];
        U32 m = 0;
        if (pfx == 0) { for (int i = 0; i < 4; i++) m |= (v.d[i] >> 31) << i; }
        else { for (int i = 0; i < 2; i++) m |= (U32)(v.q[i] >> 63) << i; }
        setReg(regField, 32, m);
        return true;
    }
    case 0x51: case 0x52: case 0x53: {
        decodeModrm();
        if (pfx == 0 || pfx == 1) {
            LOAD128(s);
            if (pfx == 0) {
                for (int i = 0; i < 4; i++) {
                    float v = s.f[i];
                    D.f[i] = op == 0x51 ? sqrtf(v) : (op == 0x52 ? 1.0f / sqrtf(v) : 1.0f / v);
                }
            } else {
                for (int i = 0; i < 2; i++) D.fd[i] = sqrt(s.fd[i]);
            }
        } else if (pfx == 2) {
            U32 bv = LOAD32();
            float v;
            memcpy(&v, &bv, 4);
            D.f[0] = op == 0x51 ? sqrtf(v) : (op == 0x52 ? 1.0f / sqrtf(v) : 1.0f / v);
        } else {
            U64 bv = LOAD64();
            double v;
            memcpy(&v, &bv, 8);
            D.fd[0] = sqrt(v);
        }
        return true;
    }
    case 0x54: case 0x55: case 0x56: case 0x57:
        decodeModrm();
        LOAD128(s);
        for (int i = 0; i < 2; i++) {
            switch (op) {
            case 0x54: D.q[i] &= s.q[i]; break;
            case 0x55: D.q[i] = ~D.q[i] & s.q[i]; break;
            case 0x56: D.q[i] |= s.q[i]; break;
            default: D.q[i] ^= s.q[i]; break;
            }
        }
        return true;
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        decodeModrm();
        if (pfx == 0) {
            LOAD128(s);
            for (int i = 0; i < 4; i++) D.f[i] = fop<float>(op, D.f[i], s.f[i]);
        } else if (pfx == 1) {
            LOAD128(s);
            for (int i = 0; i < 2; i++) D.fd[i] = fop<double>(op, D.fd[i], s.fd[i]);
        } else if (pfx == 2) {
            U32 bv = LOAD32();
            float v;
            memcpy(&v, &bv, 4);
            D.f[0] = fop<float>(op, D.f[0], v);
        } else {
            U64 bv = LOAD64();
            double v;
            memcpy(&v, &bv, 8);
            D.fd[0] = fop<double>(op, D.fd[0], v);
        }
        return true;
    case 0x5A:
        decodeModrm();
        if (pfx == 0) { // CVTPS2PD
            U64 bv = LOAD64();
            memcpy(&s.q[0], &bv, 8);
            res.fd[0] = s.f[0];
            res.fd[1] = s.f[1];
            D = res;
        } else if (pfx == 1) { // CVTPD2PS
            LOAD128(s);
            res.f[0] = (float)s.fd[0];
            res.f[1] = (float)s.fd[1];
            res.q[1] = 0;
            D = res;
        } else if (pfx == 2) { // CVTSS2SD
            U32 bv = LOAD32();
            float v;
            memcpy(&v, &bv, 4);
            D.fd[0] = v;
        } else { // CVTSD2SS
            U64 bv = LOAD64();
            double v;
            memcpy(&v, &bv, 8);
            D.f[0] = (float)v;
        }
        return true;
    case 0x5B:
        decodeModrm();
        LOAD128(s);
        if (pfx == 0) {
            for (int i = 0; i < 4; i++) D.f[i] = (float)s.sd[i];
        } else {
            for (int i = 0; i < 4; i++) D.sd[i] = (S32)cvtToInt(s.f[i], 32, mxcsr, pfx == 2);
        }
        return true;
    case 0x6E: {
        if (pfx != 1) return false;
        decodeModrm();
        U64 v = (rex & 8) ? readRM(64) : readRM(32);
        D.q[0] = v;
        D.q[1] = 0;
        return true;
    }
    case 0x6F:
        if (pfx == 0) return false;
        decodeModrm();
        LOAD128(s);
        D = s;
        return true;
    case 0x70: {
        if (pfx == 0) return false;
        decodeModrm();
        U8 imm = fetch8();
        LOAD128(s);
        if (pfx == 1) {
            for (int i = 0; i < 4; i++) res.d[i] = s.d[(imm >> (i * 2)) & 3];
        } else if (pfx == 2) {
            res.q[0] = s.q[0];
            for (int i = 0; i < 4; i++) res.w[4 + i] = s.w[4 + ((imm >> (i * 2)) & 3)];
        } else {
            res.q[1] = s.q[1];
            for (int i = 0; i < 4; i++) res.w[i] = s.w[(imm >> (i * 2)) & 3];
        }
        D = res;
        return true;
    }
    case 0x71: case 0x72: case 0x73: {
        if (pfx != 1) return false;
        decodeModrm();
        U8 imm = fetch8();
        int ext = (modrm >> 3) & 7;
        X64Xmm& v = x[rmField];
        if (op == 0x71) {
            for (int i = 0; i < 8; i++) {
                if (ext == 2) v.w[i] = imm > 15 ? 0 : (U16)(v.w[i] >> imm);
                else if (ext == 4) v.sw[i] = (S16)(v.sw[i] >> (imm > 15 ? 15 : imm));
                else if (ext == 6) v.w[i] = imm > 15 ? 0 : (U16)(v.w[i] << imm);
            }
        } else if (op == 0x72) {
            for (int i = 0; i < 4; i++) {
                if (ext == 2) v.d[i] = imm > 31 ? 0 : (v.d[i] >> imm);
                else if (ext == 4) v.sd[i] = v.sd[i] >> (imm > 31 ? 31 : imm);
                else if (ext == 6) v.d[i] = imm > 31 ? 0 : (v.d[i] << imm);
            }
        } else {
            if (ext == 2 || ext == 6) {
                for (int i = 0; i < 2; i++) {
                    if (imm > 63) v.q[i] = 0;
                    else v.q[i] = ext == 2 ? (v.q[i] >> imm) : (v.q[i] << imm);
                }
            } else if (ext == 3 || ext == 7) {
                U32 n = imm > 16 ? 16 : imm;
                X64Xmm t;
                memset(&t, 0, sizeof(t));
                for (int i = 0; i < 16; i++) {
                    if (ext == 3) { if (i + n < 16) t.b[i] = v.b[i + n]; }
                    else { if (i >= (int)n) t.b[i] = v.b[i - n]; }
                }
                v = t;
            } else {
                return false;
            }
        }
        return true;
    }
    case 0x77: // EMMS
        return true;
    case 0x7E:
        decodeModrm();
        if (pfx == 1) {
            if (rex & 8) writeRM(64, D.q[0]);
            else writeRM(32, D.d[0]);
            return true;
        }
        if (pfx == 2) {
            U64 v = LOAD64();
            D.q[0] = v;
            D.q[1] = 0;
            return true;
        }
        return false;
    case 0x7F:
        if (pfx == 0) return false;
        decodeModrm();
        if (isMem) writeXmm(ea(), D); else x[rmField] = D;
        return true;
    case 0xC2: {
        decodeModrm();
        // the immediate follows the modrm/displacement
        U64 addrPart = 0;
        bool mem = isMem;
        U8 imm;
        if (pfx < 2) {
            imm = fetch8();
            LOAD128(s);
            if (pfx == 0) {
                for (int i = 0; i < 4; i++) res.d[i] = fcmp<float>(imm, D.f[i], s.f[i]) ? 0xFFFFFFFF : 0;
            } else {
                for (int i = 0; i < 2; i++) res.q[i] = fcmp<double>(imm, D.fd[i], s.fd[i]) ? ~0ULL : 0;
            }
            D = res;
        } else if (pfx == 2) {
            imm = fetch8();
            U32 bv = LOAD32();
            float v;
            memcpy(&v, &bv, 4);
            D.d[0] = fcmp<float>(imm, D.f[0], v) ? 0xFFFFFFFF : 0;
        } else {
            imm = fetch8();
            U64 bv = LOAD64();
            double v;
            memcpy(&v, &bv, 8);
            D.q[0] = fcmp<double>(imm, D.fd[0], v) ? ~0ULL : 0;
        }
        (void)addrPart;
        (void)mem;
        return true;
    }
    case 0xC4: {
        if (pfx != 1) return false;
        decodeModrm();
        U8 imm = fetch8();
        U16 v = isMem ? read16(ea()) : (U16)r[rmField];
        D.w[imm & 7] = v;
        return true;
    }
    case 0xC5: {
        if (pfx != 1) return false;
        decodeModrm();
        U8 imm = fetch8();
        setReg(regField, 32, x[rmField].w[imm & 7]);
        return true;
    }
    case 0xC6: {
        decodeModrm();
        U8 imm = fetch8();
        LOAD128(s);
        if (pfx == 0) {
            res.d[0] = D.d[imm & 3];
            res.d[1] = D.d[(imm >> 2) & 3];
            res.d[2] = s.d[(imm >> 4) & 3];
            res.d[3] = s.d[(imm >> 6) & 3];
        } else {
            res.q[0] = D.q[imm & 1];
            res.q[1] = s.q[(imm >> 1) & 1];
        }
        D = res;
        return true;
    }
    case 0xD6:
        if (pfx != 1) return false;
        decodeModrm();
        if (isMem) write64(ea(), D.q[0]);
        else { x[rmField].q[0] = D.q[0]; x[rmField].q[1] = 0; }
        return true;
    case 0xD7: {
        if (pfx != 1) return false;
        decodeModrm();
        U32 m = 0;
        for (int i = 0; i < 16; i++) m |= (U32)(x[rmField].b[i] >> 7) << i;
        setReg(regField, 32, m);
        return true;
    }
    case 0xE6:
        decodeModrm();
        if (pfx == 2) { // CVTDQ2PD
            U64 bv = LOAD64();
            memcpy(&s.q[0], &bv, 8);
            res.fd[0] = s.sd[0];
            res.fd[1] = s.sd[1];
            D = res;
            return true;
        }
        if (pfx == 1 || pfx == 3) { // CVTTPD2DQ / CVTPD2DQ
            LOAD128(s);
            res.sd[0] = (S32)cvtToInt(s.fd[0], 32, mxcsr, pfx == 1);
            res.sd[1] = (S32)cvtToInt(s.fd[1], 32, mxcsr, pfx == 1);
            res.q[1] = 0;
            D = res;
            return true;
        }
        return false;
    case 0xE7:
        if (pfx != 1) return false;
        decodeModrm();
        if (isMem) writeXmm(ea(), D);
        return true;
    default:
        break;
    }

    // remaining packed integer ops (66 prefix, xmm, xmm/m128)
    if (pfx != 1) {
        return false;
    }
    bool integerOp = (op >= 0x60 && op <= 0x6D) || (op >= 0x74 && op <= 0x76) || (op >= 0xD1 && op <= 0xFE);
    if (!integerOp) {
        return false;
    }
    decodeModrm();
    LOAD128(s);
    X64Xmm a = D;
    memset(&res, 0, sizeof(res));
    int i;
    switch (op) {
    case 0x60: for (i = 0; i < 8; i++) { res.b[2 * i] = a.b[i]; res.b[2 * i + 1] = s.b[i]; } break;
    case 0x61: for (i = 0; i < 4; i++) { res.w[2 * i] = a.w[i]; res.w[2 * i + 1] = s.w[i]; } break;
    case 0x62: for (i = 0; i < 2; i++) { res.d[2 * i] = a.d[i]; res.d[2 * i + 1] = s.d[i]; } break;
    case 0x63: for (i = 0; i < 8; i++) { res.sb[i] = satS8(a.sw[i]); res.sb[8 + i] = satS8(s.sw[i]); } break;
    case 0x64: for (i = 0; i < 16; i++) res.b[i] = a.sb[i] > s.sb[i] ? 0xFF : 0; break;
    case 0x65: for (i = 0; i < 8; i++) res.w[i] = a.sw[i] > s.sw[i] ? 0xFFFF : 0; break;
    case 0x66: for (i = 0; i < 4; i++) res.d[i] = a.sd[i] > s.sd[i] ? 0xFFFFFFFF : 0; break;
    case 0x67: for (i = 0; i < 8; i++) { res.b[i] = satU8(a.sw[i]); res.b[8 + i] = satU8(s.sw[i]); } break;
    case 0x68: for (i = 0; i < 8; i++) { res.b[2 * i] = a.b[8 + i]; res.b[2 * i + 1] = s.b[8 + i]; } break;
    case 0x69: for (i = 0; i < 4; i++) { res.w[2 * i] = a.w[4 + i]; res.w[2 * i + 1] = s.w[4 + i]; } break;
    case 0x6A: for (i = 0; i < 2; i++) { res.d[2 * i] = a.d[2 + i]; res.d[2 * i + 1] = s.d[2 + i]; } break;
    case 0x6B: for (i = 0; i < 4; i++) { res.sw[i] = satS16(a.sd[i]); res.sw[4 + i] = satS16(s.sd[i]); } break;
    case 0x6C: res.q[0] = a.q[0]; res.q[1] = s.q[0]; break;
    case 0x6D: res.q[0] = a.q[1]; res.q[1] = s.q[1]; break;
    case 0x74: for (i = 0; i < 16; i++) res.b[i] = a.b[i] == s.b[i] ? 0xFF : 0; break;
    case 0x75: for (i = 0; i < 8; i++) res.w[i] = a.w[i] == s.w[i] ? 0xFFFF : 0; break;
    case 0x76: for (i = 0; i < 4; i++) res.d[i] = a.d[i] == s.d[i] ? 0xFFFFFFFF : 0; break;
    case 0xD1: { U64 c = s.q[0]; for (i = 0; i < 8; i++) res.w[i] = c > 15 ? 0 : (U16)(a.w[i] >> c); break; }
    case 0xD2: { U64 c = s.q[0]; for (i = 0; i < 4; i++) res.d[i] = c > 31 ? 0 : (a.d[i] >> c); break; }
    case 0xD3: { U64 c = s.q[0]; for (i = 0; i < 2; i++) res.q[i] = c > 63 ? 0 : (a.q[i] >> c); break; }
    case 0xD4: for (i = 0; i < 2; i++) res.q[i] = a.q[i] + s.q[i]; break;
    case 0xD5: for (i = 0; i < 8; i++) res.w[i] = (U16)((S32)a.sw[i] * (S32)s.sw[i]); break;
    case 0xD8: for (i = 0; i < 16; i++) res.b[i] = satU8((S32)a.b[i] - (S32)s.b[i]); break;
    case 0xD9: for (i = 0; i < 8; i++) res.w[i] = satU16((S32)a.w[i] - (S32)s.w[i]); break;
    case 0xDA: for (i = 0; i < 16; i++) res.b[i] = a.b[i] < s.b[i] ? a.b[i] : s.b[i]; break;
    case 0xDB: for (i = 0; i < 2; i++) res.q[i] = a.q[i] & s.q[i]; break;
    case 0xDC: for (i = 0; i < 16; i++) res.b[i] = satU8((S32)a.b[i] + (S32)s.b[i]); break;
    case 0xDD: for (i = 0; i < 8; i++) res.w[i] = satU16((S32)a.w[i] + (S32)s.w[i]); break;
    case 0xDE: for (i = 0; i < 16; i++) res.b[i] = a.b[i] > s.b[i] ? a.b[i] : s.b[i]; break;
    case 0xDF: for (i = 0; i < 2; i++) res.q[i] = ~a.q[i] & s.q[i]; break;
    case 0xE0: for (i = 0; i < 16; i++) res.b[i] = (U8)(((U32)a.b[i] + s.b[i] + 1) >> 1); break;
    case 0xE1: { U64 c = s.q[0] > 15 ? 15 : s.q[0]; for (i = 0; i < 8; i++) res.sw[i] = (S16)(a.sw[i] >> c); break; }
    case 0xE2: { U64 c = s.q[0] > 31 ? 31 : s.q[0]; for (i = 0; i < 4; i++) res.sd[i] = a.sd[i] >> c; break; }
    case 0xE3: for (i = 0; i < 8; i++) res.w[i] = (U16)(((U32)a.w[i] + s.w[i] + 1) >> 1); break;
    case 0xE4: for (i = 0; i < 8; i++) res.w[i] = (U16)(((U32)a.w[i] * (U32)s.w[i]) >> 16); break;
    case 0xE5: for (i = 0; i < 8; i++) res.w[i] = (U16)(((S32)a.sw[i] * (S32)s.sw[i]) >> 16); break;
    case 0xE8: for (i = 0; i < 16; i++) res.sb[i] = satS8((S32)a.sb[i] - (S32)s.sb[i]); break;
    case 0xE9: for (i = 0; i < 8; i++) res.sw[i] = satS16((S32)a.sw[i] - (S32)s.sw[i]); break;
    case 0xEA: for (i = 0; i < 8; i++) res.sw[i] = a.sw[i] < s.sw[i] ? a.sw[i] : s.sw[i]; break;
    case 0xEB: for (i = 0; i < 2; i++) res.q[i] = a.q[i] | s.q[i]; break;
    case 0xEC: for (i = 0; i < 16; i++) res.sb[i] = satS8((S32)a.sb[i] + (S32)s.sb[i]); break;
    case 0xED: for (i = 0; i < 8; i++) res.sw[i] = satS16((S32)a.sw[i] + (S32)s.sw[i]); break;
    case 0xEE: for (i = 0; i < 8; i++) res.sw[i] = a.sw[i] > s.sw[i] ? a.sw[i] : s.sw[i]; break;
    case 0xEF: for (i = 0; i < 2; i++) res.q[i] = a.q[i] ^ s.q[i]; break;
    case 0xF1: { U64 c = s.q[0]; for (i = 0; i < 8; i++) res.w[i] = c > 15 ? 0 : (U16)(a.w[i] << c); break; }
    case 0xF2: { U64 c = s.q[0]; for (i = 0; i < 4; i++) res.d[i] = c > 31 ? 0 : (a.d[i] << c); break; }
    case 0xF3: { U64 c = s.q[0]; for (i = 0; i < 2; i++) res.q[i] = c > 63 ? 0 : (a.q[i] << c); break; }
    case 0xF4: res.q[0] = (U64)a.d[0] * s.d[0]; res.q[1] = (U64)a.d[2] * s.d[2]; break;
    case 0xF5: for (i = 0; i < 4; i++) res.sd[i] = (S32)a.sw[2 * i] * s.sw[2 * i] + (S32)a.sw[2 * i + 1] * s.sw[2 * i + 1]; break;
    case 0xF6: {
        for (int h = 0; h < 2; h++) {
            U32 sum = 0;
            for (i = 0; i < 8; i++) {
                S32 d = (S32)a.b[h * 8 + i] - (S32)s.b[h * 8 + i];
                sum += d < 0 ? -d : d;
            }
            res.q[h] = sum;
        }
        break;
    }
    case 0xF8: for (i = 0; i < 16; i++) res.b[i] = a.b[i] - s.b[i]; break;
    case 0xF9: for (i = 0; i < 8; i++) res.w[i] = a.w[i] - s.w[i]; break;
    case 0xFA: for (i = 0; i < 4; i++) res.d[i] = a.d[i] - s.d[i]; break;
    case 0xFB: for (i = 0; i < 2; i++) res.q[i] = a.q[i] - s.q[i]; break;
    case 0xFC: for (i = 0; i < 16; i++) res.b[i] = a.b[i] + s.b[i]; break;
    case 0xFD: for (i = 0; i < 8; i++) res.w[i] = a.w[i] + s.w[i]; break;
    case 0xFE: for (i = 0; i < 4; i++) res.d[i] = a.d[i] + s.d[i]; break;
    default:
        unhandled("SSE2 integer", op);
        return true;
    }
    D = res;
    return true;
#undef LOAD128
#undef LOAD64
#undef LOAD32
#undef D
}

void X64CPU::step0F38() {
    U8 op = fetch8();
    unhandled("0F 38", op);
}

void X64CPU::step0F3A() {
    U8 op = fetch8();
    unhandled("0F 3A", op);
}
