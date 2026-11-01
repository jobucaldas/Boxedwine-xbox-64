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
#include "../common/common_fpu.h"

// x87 instructions reuse the existing BoxedWine FPU implementation. Those
// helpers take a CPU* and a 32-bit guest address, which works because the
// 64-bit guest keeps all memory below 4GB. Flags are bridged between the
// x64 lazy flags and the base CPU flags for FCOMI/FCMOV.

typedef void (*FpuMemOp)(CPU* cpu, U32 address);
typedef void (*FpuRegOp)(CPU* cpu, U32 reg);

static const FpuMemOp memOps[8][8] = {
    { common_FADD_SINGLE_REAL, common_FMUL_SINGLE_REAL, common_FCOM_SINGLE_REAL, common_FCOM_SINGLE_REAL_Pop, common_FSUB_SINGLE_REAL, common_FSUBR_SINGLE_REAL, common_FDIV_SINGLE_REAL, common_FDIVR_SINGLE_REAL },
    { common_FLD_SINGLE_REAL, nullptr, common_FST_SINGLE_REAL, common_FST_SINGLE_REAL_Pop, common_FLDENV, common_FLDCW, common_FNSTENV, common_FNSTCW },
    { common_FIADD_DWORD_INTEGER, common_FIMUL_DWORD_INTEGER, common_FICOM_DWORD_INTEGER, common_FICOM_DWORD_INTEGER_Pop, common_FISUB_DWORD_INTEGER, common_FISUBR_DWORD_INTEGER, common_FIDIV_DWORD_INTEGER, common_FIDIVR_DWORD_INTEGER },
    { common_FILD_DWORD_INTEGER, common_FISTTP32, common_FIST_DWORD_INTEGER, common_FIST_DWORD_INTEGER_Pop, nullptr, common_FLD_EXTENDED_REAL, nullptr, common_FSTP_EXTENDED_REAL },
    { common_FADD_DOUBLE_REAL, common_FMUL_DOUBLE_REAL, common_FCOM_DOUBLE_REAL, common_FCOM_DOUBLE_REAL_Pop, common_FSUB_DOUBLE_REAL, common_FSUBR_DOUBLE_REAL, common_FDIV_DOUBLE_REAL, common_FDIVR_DOUBLE_REAL },
    { common_FLD_DOUBLE_REAL, common_FISTTP64, common_FST_DOUBLE_REAL, common_FST_DOUBLE_REAL_Pop, common_FRSTOR, nullptr, common_FNSAVE, common_FNSTSW },
    { common_FIADD_WORD_INTEGER, common_FIMUL_WORD_INTEGER, common_FICOM_WORD_INTEGER, common_FICOM_WORD_INTEGER_Pop, common_FISUB_WORD_INTEGER, common_FISUBR_WORD_INTEGER, common_FIDIV_WORD_INTEGER, common_FIDIVR_WORD_INTEGER },
    { common_FILD_WORD_INTEGER, common_FISTTP16, common_FIST_WORD_INTEGER, common_FIST_WORD_INTEGER_Pop, common_FBLD_PACKED_BCD, common_FILD_QWORD_INTEGER, common_FBSTP_PACKED_BCD, common_FISTP_QWORD_INTEGER },
};

static const FpuRegOp regOps[8][8] = {
    { common_FADD_ST0_STj, common_FMUL_ST0_STj, common_FCOM_STi, common_FCOM_STi_Pop, common_FSUB_ST0_STj, common_FSUBR_ST0_STj, common_FDIV_ST0_STj, common_FDIVR_ST0_STj },
    { common_FLD_STi, common_FXCH_STi, nullptr, common_FST_STi_Pop, nullptr, nullptr, nullptr, nullptr },
    { common_FCMOV_ST0_STj_CF, common_FCMOV_ST0_STj_ZF, common_FCMOV_ST0_STj_CF_OR_ZF, common_FCMOV_ST0_STj_PF, nullptr, nullptr, nullptr, nullptr },
    { common_FCMOV_ST0_STj_NCF, common_FCMOV_ST0_STj_NZF, common_FCMOV_ST0_STj_NCF_AND_NZF, common_FCMOV_ST0_STj_NPF, nullptr, common_FUCOMI_ST0_STj, common_FCOMI_ST0_STj, nullptr },
    { common_FADD_STi_ST0, common_FMUL_STi_ST0, common_FCOM_STi, common_FCOM_STi_Pop, common_FSUBR_STi_ST0, common_FSUB_STi_ST0, common_FDIVR_STi_ST0, common_FDIV_STi_ST0 },
    { common_FFREE_STi, common_FXCH_STi, common_FST_STi, common_FST_STi_Pop, common_FUCOM_STi, common_FUCOM_STi_Pop, nullptr, nullptr },
    { common_FADD_STi_ST0_Pop, common_FMUL_STi_ST0_Pop, common_FCOM_STi_Pop, nullptr, common_FSUBR_STi_ST0_Pop, common_FSUB_STi_ST0_Pop, common_FDIVR_STi_ST0_Pop, common_FDIV_STi_ST0_Pop },
    { common_FFREEP_STi, common_FXCH_STi, common_FST_STi_Pop, common_FST_STi_Pop, nullptr, common_FUCOMI_ST0_STj_Pop, common_FCOMI_ST0_STj_Pop, nullptr },
};

typedef void (*FpuOp0)(CPU* cpu);

static const FpuOp0 d9Ops[32] = {
    // E0..E7
    common_FCHS, common_FABS, nullptr, nullptr, common_FTST, common_FXAM, nullptr, nullptr,
    // E8..EF
    common_FLD1, common_FLDL2T, common_FLDL2E, common_FLDPI, common_FLDLG2, common_FLDLN2, common_FLDZ, nullptr,
    // F0..F7
    common_F2XM1, common_FYL2X, common_FPTAN, common_FPATAN, common_FXTRACT, common_FPREM_nearest, common_FDECSTP, common_FINCSTP,
    // F8..FF
    common_FPREM, common_FYL2XP1, common_FSQRT, common_FSINCOS, common_FRNDINT, common_FSCALE, common_FSIN, common_FCOS,
};

void X64CPU::stepFPU(U8 op) {
    decodeModrm();
    int group = op - 0xD8;
    int ext = (modrm >> 3) & 7;
    if (isMem) {
        FpuMemOp f = memOps[group][ext];
        if (!f) {
            unhandled("x87 memory", (op << 8) | modrm);
            return;
        }
        U64 address = ea();
        if (address >> 32) {
            fault(address, false);
        }
        f(this, (U32)address);
        return;
    }
    U32 reg = modrm & 7;
    if (op == 0xD9 && modrm >= 0xE0) {
        FpuOp0 f = d9Ops[modrm - 0xE0];
        if (!f) {
            unhandled("x87", (op << 8) | modrm);
            return;
        }
        f(this);
        return;
    }
    if (op == 0xD9 && modrm == 0xD0) {
        return; // FNOP
    }
    if (op == 0xDA && modrm == 0xE9) {
        common_FUCOMPP(this);
        return;
    }
    if (op == 0xDB && modrm == 0xE2) {
        common_FNCLEX(this);
        return;
    }
    if (op == 0xDB && modrm == 0xE3) {
        common_FNINIT(this);
        return;
    }
    if (op == 0xDE && modrm == 0xD9) {
        common_FCOMPP(this);
        return;
    }
    if (op == 0xDF && modrm == 0xE0) {
        common_FNSTSW_AX(this);
        r[0] = (r[0] & ~0xFFFFULL) | CPU::reg[0].word[0];
        return;
    }
    FpuRegOp f = regOps[group][ext];
    if (!f) {
        unhandled("x87 register", (op << 8) | modrm);
        return;
    }
    bool flagOp = (op == 0xDA || op == 0xDB || op == 0xDF) && ext != 0 && !(op == 0xDF && ext < 4);
    if (op == 0xDA || op == 0xDB) {
        flagOp = true;
    }
    if (flagOp) {
        CPU::flags = getFlags();
        lazyFlagType = FLAGS_NONE;
    }
    f(this, reg);
    if (flagOp) {
        fillFlags();
        rflags = (rflags & ~(CF | PF | AF | ZF | SF | OF)) | (CPU::flags & (CF | PF | AF | ZF | SF | OF));
    }
}
