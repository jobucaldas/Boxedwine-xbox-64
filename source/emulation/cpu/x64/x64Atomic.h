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

#ifndef __X64_ATOMIC_H__
#define __X64_ATOMIC_H__

// Host atomics on guest RAM for LOCK-prefixed instructions. Guest threads run
// on separate host threads, so locked read-modify-write ops are implemented as
// an atomic load followed by a compare-and-swap of the computed result.

#ifdef _MSC_VER
#include <intrin.h>
static inline U64 x64AtomicLoad(U8* p, int bits) {
    switch (bits) {
    case 8: return (U8)_InterlockedOr8((volatile char*)p, 0);
    case 16: return (U16)_InterlockedOr16((volatile short*)p, 0);
    case 32: return (U32)_InterlockedOr((volatile long*)p, 0);
    default: return (U64)_InterlockedOr64((volatile __int64*)p, 0);
    }
}

static inline bool x64AtomicCas(U8* p, int bits, U64 expected, U64 desired) {
    switch (bits) {
    case 8: return (U8)_InterlockedCompareExchange8((volatile char*)p, (char)desired, (char)expected) == (U8)expected;
    case 16: return (U16)_InterlockedCompareExchange16((volatile short*)p, (short)desired, (short)expected) == (U16)expected;
    case 32: return (U32)_InterlockedCompareExchange((volatile long*)p, (long)desired, (long)expected) == (U32)expected;
    default: return (U64)_InterlockedCompareExchange64((volatile __int64*)p, (__int64)desired, (__int64)expected) == expected;
    }
}

static inline bool x64AtomicCas128(U8* p, U64* expected, U64 desiredLo, U64 desiredHi) {
    return _InterlockedCompareExchange128((volatile __int64*)p, (__int64)desiredHi, (__int64)desiredLo, (__int64*)expected) != 0;
}
#else
static inline U64 x64AtomicLoad(U8* p, int bits) {
    switch (bits) {
    case 8: return __atomic_load_n(p, __ATOMIC_SEQ_CST);
    case 16: return __atomic_load_n((U16*)p, __ATOMIC_SEQ_CST);
    case 32: return __atomic_load_n((U32*)p, __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((U64*)p, __ATOMIC_SEQ_CST);
    }
}

static inline bool x64AtomicCas(U8* p, int bits, U64 expected, U64 desired) {
    switch (bits) {
    case 8: { U8 e = (U8)expected; return __atomic_compare_exchange_n(p, &e, (U8)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 16: { U16 e = (U16)expected; return __atomic_compare_exchange_n((U16*)p, &e, (U16)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 32: { U32 e = (U32)expected; return __atomic_compare_exchange_n((U32*)p, &e, (U32)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    default: { U64 e = expected; return __atomic_compare_exchange_n((U64*)p, &e, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    }
}

// expected[0..1] = lo/hi; on failure they receive the current value
static inline bool x64AtomicCas128(U8* p, U64* expected, U64 desiredLo, U64 desiredHi) {
#if defined(__x86_64__)
    bool ok;
    U64 lo = expected[0], hi = expected[1];
    __asm__ __volatile__("lock cmpxchg16b %1\n\tsete %0"
        : "=q"(ok), "+m"(*(volatile U64(*)[2])p), "+a"(lo), "+d"(hi)
        : "b"(desiredLo), "c"(desiredHi)
        : "memory", "cc");
    expected[0] = lo;
    expected[1] = hi;
    return ok;
#else
    unsigned __int128 e = ((unsigned __int128)expected[1] << 64) | expected[0];
    unsigned __int128 d = ((unsigned __int128)desiredHi << 64) | desiredLo;
    bool ok = __atomic_compare_exchange_n((unsigned __int128*)p, &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    expected[0] = (U64)e;
    expected[1] = (U64)(e >> 64);
    return ok;
#endif
}
#endif

#endif
