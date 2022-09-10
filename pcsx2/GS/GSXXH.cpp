/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include "PrecompiledHeader.h"
#include "MultiISA.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
// Prefetch instructions break forwarding of data in registers
#define XXH_NO_PREFETCH
namespace CURRENT_ISA // XXH doesn't seem to use symbols that allow the compiler to deduplicate, but just in case...
{
#include <xxhash.h>
}

MULTI_ISA_UNSHARED_IMPL;

// Include this after xxhash so we can add namespaces (GSXXH is set up to not include xxhash header if it's already been included)
#include "GSXXH.h"

u64 __noinline CURRENT_ISA::GSXXH3_64_Long(const void* data, size_t len)
{
	// XXH marks its function that calls this noinline, and it would be silly to stack noinline functions, so call the internal function directly
	return XXH3_hashLong_64b_internal(data, len, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_accumulate_512, XXH3_scrambleAcc);
}

u32 CURRENT_ISA::GSXXH3_64_Update(void* state, const void* data, size_t len)
{
	return XXH3_64bits_update(static_cast<XXH3_state_t*>(state), static_cast<const xxh_u8*>(data), len);
}

u64 CURRENT_ISA::GSXXH3_64_Digest(void* state)
{
	return XXH3_64bits_digest(static_cast<XXH3_state_t*>(state));
}

#if _M_SSE < 0x501
namespace CURRENT_ISA
{
static u64 __forceinline GSXXH3_block_rgb(const void* data)
{
	const __m128i* vdata = static_cast<const __m128i*>(data);
	__m128i block[16];
	for (size_t i = 0; i < std::size(block); i++)
		block[i] = _mm_and_si128(vdata[i], _mm_set1_epi32(0x00ffffff));
	return XXH3_hashLong_64b_internal(block, sizeof(block), XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_accumulate_512, XXH3_scrambleAcc);
}

static u64 __noinline GSXXH3_block_alpha(const void* data)
{
	const __m128i* vdata = static_cast<const __m128i*>(data);
	__m128i block[4];
	for (size_t i = 0; i < std::size(block); i++) {
		__m128i x0 = _mm_srli_epi32(vdata[i * 4 + 0], 24);
		__m128i x1 = _mm_srli_epi32(vdata[i * 4 + 1], 24);
		__m128i x2 = _mm_srli_epi32(vdata[i * 4 + 2], 24);
		__m128i x3 = _mm_srli_epi32(vdata[i * 4 + 3], 24);
		x0 = _mm_packs_epi32(x0, x1);
		x1 = _mm_packs_epi32(x2, x3);
		block[i] = _mm_packus_epi16(x0, x1);
	}
	return XXH3_64bits_internal(block, sizeof(block), 0, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_hashLong_64b_default);
}
}
#endif

GSBlockHash __noinline CURRENT_ISA::GSXXH3_GSBlock(const void* data)
{
	GSBlockHash output;
#if _M_SSE >= 0x501
	// The entire block fits in 8 AVX2 registers, so no spilling will happen
	const __m256i* vdata = static_cast<const __m256i*>(data);
	__m128i alpha[4];
	__m256i rgb[8];
	for (size_t i = 0; i < 2; i++) {
		__m256i y0 = _mm256_srli_epi32(vdata[i * 4 + 0], 24);
		__m256i y1 = _mm256_srli_epi32(vdata[i * 4 + 1], 24);
		__m256i y2 = _mm256_srli_epi32(vdata[i * 4 + 2], 24);
		__m256i y3 = _mm256_srli_epi32(vdata[i * 4 + 3], 24);
		y0 = _mm256_packs_epi32(y0, y1);
		y1 = _mm256_packs_epi32(y2, y3);
		y0 = _mm256_packus_epi16(y0, y1);
		__m128i x0 = _mm256_castsi256_si128(y0);
		__m128i x1 = _mm256_extracti128_si256(y0, 1);
		alpha[i * 2 + 0] = _mm_unpacklo_epi32(x0, x1);
		alpha[i * 2 + 1] = _mm_unpackhi_epi32(x0, x1);
	}
	for (size_t i = 0; i < std::size(rgb); i++) {
		rgb[i] = _mm256_and_si256(vdata[i], _mm256_set1_epi32(0x00ffffff));
	}
	output.rgb = XXH3_hashLong_64b_internal(rgb, sizeof(rgb), XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_accumulate_512, XXH3_scrambleAcc);
	output.alpha = XXH3_64bits_internal(alpha, sizeof(alpha), 0, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_hashLong_64b_default);
#else
	// If we try to do both at once, the compiler will try to hold the entire block in registers, since it's needed for both rgb and alpha
	// That requires all 16 registers, leaving none for calculations, and as a result, the compiler will spill them all over the stack
	// Avoid that by marking alpha as noinline, so the two calculations don't try to share registers
	output.alpha = GSXXH3_block_alpha(data);
	output.rgb = GSXXH3_block_rgb(data);
#endif
	return output;
}
