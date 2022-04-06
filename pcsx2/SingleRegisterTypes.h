/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

// --------------------------------------------------------------------------------------
//  r64 / r128 - Types that are guaranteed to fit in one register
// --------------------------------------------------------------------------------------
// Note: Recompilers rely on some of these types and the registers they allocate to,
// so be careful if you want to change them

#pragma once

#include <cstring>
#include <immintrin.h>
#include <emmintrin.h>

// Can't stick them in structs because it breaks calling convention things, yay
using r128 = __m128i;
#ifdef _M_X86_64
using r64 = u64;
#else
using r64 = __m128i;
#endif

// Calling convention setting, yay
#define RETURNS_R128 r128 __vectorcall
#define TAKES_R128 __vectorcall
#ifdef _M_X86_64
	#define RETURNS_R64 r64
	#define TAKES_R64
#else
	#define RETURNS_R64 r64 __vectorcall
	#define TAKES_R64 __vectorcall
#endif

// And since we can't stick them in structs, we get lots of static methods, yay!

__forceinline static r64 r64_load(const void* ptr)
{
#ifdef _M_X86_64
	return *reinterpret_cast<const u64*>(ptr);
#else
	return _mm_loadl_epi64(reinterpret_cast<const r64*>(ptr));
#endif
}

__forceinline static void r64_store(void* ptr, r64 val)
{
#ifdef _M_X86_64
	*reinterpret_cast<u64*>(ptr) = val;
#else
	return _mm_storel_epi64(reinterpret_cast<r64*>(ptr), val);
#endif
}

__forceinline static r64 r64_zero()
{
#ifdef _M_X86_64
	return 0;
#else
	return _mm_setzero_si128();
#endif
}

__forceinline static r64 r64_from_u32(u32 val)
{
#ifdef _M_X86_64
	return val;
#else
	return _mm_cvtsi32_si128(val);
#endif
}

__forceinline static r64 r64_from_u32x2(u32 lo, u32 hi)
{
#ifdef _M_X86_64
	return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
#else
	return _mm_unpacklo_epi32(_mm_cvtsi32_si128(lo), _mm_cvtsi32_si128(hi));
#endif
}

__forceinline static r64 r64_from_u64(u64 val)
{
#ifdef _M_X86_64
	return val;
#else
	return r64_from_u32x2(val, val >> 32);
#endif
}

__forceinline static u64 r64_to_u64(r64 val)
{
#ifdef _M_X86_64
	return val;
#else
	u32 lo = _mm_cvtsi128_si32(val);
	u32 hi = _mm_cvtsi128_si32(_mm_shuffle_epi32(val, _MM_SHUFFLE(1, 1, 1, 1)));
	return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
#endif
}

__forceinline static r128 r128_load(const void* ptr)
{
	return _mm_load_si128(reinterpret_cast<const r128*>(ptr));
}

__forceinline static void r128_store(void* ptr, r128 val)
{
	return _mm_store_si128(reinterpret_cast<r128*>(ptr), val);
}

__forceinline static void r128_store_unaligned(void* ptr, r128 val)
{
	return _mm_storeu_si128(reinterpret_cast<r128*>(ptr), val);
}

__forceinline static r128 r128_zero()
{
	return _mm_setzero_si128();
}

/// Expects that r64 came from r64-handling code, and not from a recompiler or something
__forceinline static r128 r128_from_u64_dup(u64 val)
{
	return _mm_set1_epi64x(val);
}
__forceinline static r128 r128_from_u64_zext(u64 val)
{
	return _mm_set_epi64x(0, val);
}

__forceinline static r128 r128_from_u32x4(u32 lo0, u32 lo1, u32 hi0, u32 hi1)
{
	return _mm_setr_epi32(lo0, lo1, hi0, hi1);
}

__forceinline static r128 r128_from_u128(const u128& u)
{
	return _mm_loadu_si128(reinterpret_cast<const __m128i*>(&u));
}

__forceinline static u32 r128_to_u32(r128 val)
{
	return _mm_cvtsi128_si32(val);
}

__forceinline static u64 r128_to_u64(r128 val)
{
#ifdef _M_X86_64
	return _mm_cvtsi128_si64(val);
#else
	return r64_to_u64(val);
#endif
}

__forceinline static RETURNS_R64 r128_to_r64(r128 val)
{
#ifdef _M_X86_64
	return r128_to_u64(val);
#else
	return val;
#endif
}

__forceinline static u128 r128_to_u128(r128 val)
{
	alignas(16) u128 ret;
	_mm_store_si128(reinterpret_cast<r128*>(&ret), val);
	return ret;
}

template <typename u>
struct rhelper;

template <>
struct rhelper<u128>
{
	using r = r128;
	__forceinline static r load(void* ptr) { return r128_load(ptr); }
	__forceinline static r zero() { return r128_zero(); }
};

template <>
struct rhelper<u64>
{
	using r = r64;
	__forceinline static r load(void* ptr) { return r64_load(ptr); }
	__forceinline static r zero() { return r64_zero(); }
};

template <typename u>
using u_to_r = typename rhelper<u>::r;
