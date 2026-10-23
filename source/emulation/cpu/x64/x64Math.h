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

#ifndef __X64_MATH_H__
#define __X64_MATH_H__

// Portable 128-bit multiply/divide helpers (MSVC has no __int128).

static inline void x64_umul128(U64 a, U64 b, U64* lo, U64* hi) {
    U64 aLo = (U32)a, aHi = a >> 32, bLo = (U32)b, bHi = b >> 32;
    U64 p0 = aLo * bLo;
    U64 p1 = aLo * bHi;
    U64 p2 = aHi * bLo;
    U64 p3 = aHi * bHi;
    U64 mid = (p0 >> 32) + (U32)p1 + (U32)p2;
    *lo = (mid << 32) | (U32)p0;
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

static inline void x64_smul128(S64 a, S64 b, U64* lo, S64* hi) {
    U64 h;
    x64_umul128((U64)a, (U64)b, lo, &h);
    if (a < 0) h -= (U64)b;
    if (b < 0) h -= (U64)a;
    *hi = (S64)h;
}

// (hi:lo) / d, returns false on divide overflow
static inline bool x64_udiv128(U64 hi, U64 lo, U64 d, U64* q, U64* rem) {
    if (hi >= d) {
        return false;
    }
    if (hi == 0) {
        *q = lo / d;
        *rem = lo % d;
        return true;
    }
    U64 quotient = 0;
    U64 r = hi;
    for (int i = 63; i >= 0; i--) {
        bool top = (r >> 63) != 0;
        r = (r << 1) | ((lo >> i) & 1);
        if (top || r >= d) {
            r -= d;
            quotient |= 1ULL << i;
        }
    }
    *q = quotient;
    *rem = r;
    return true;
}

static inline bool x64_sdiv128(S64 hi, U64 lo, S64 d, S64* q, S64* rem) {
    bool negN = hi < 0;
    bool negD = d < 0;
    U64 nHi = (U64)hi, nLo = lo;
    if (negN) {
        nLo = ~nLo + 1;
        nHi = ~nHi + (nLo == 0 ? 1 : 0);
    }
    U64 ud = negD ? (U64)0 - (U64)d : (U64)d;
    U64 uq, ur;
    if (!x64_udiv128(nHi, nLo, ud, &uq, &ur)) {
        return false;
    }
    if (negN != negD) {
        if (uq > 0x8000000000000000ULL) return false;
        *q = (S64)(0 - uq);
    } else {
        if (uq > 0x7FFFFFFFFFFFFFFFULL) return false;
        *q = (S64)uq;
    }
    *rem = negN ? (S64)(0 - ur) : (S64)ur;
    return true;
}

#endif
