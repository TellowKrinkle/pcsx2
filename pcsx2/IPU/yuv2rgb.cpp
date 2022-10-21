/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2016  PCSX2 Dev Team
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


// IPU-correct yuv conversions by Pseudonym
// SSE2 Implementation by Pseudonym

#include "PrecompiledHeader.h"

#include "Common.h"
#include "IPU.h"
#include "yuv2rgb.h"
#include "mpeg2lib/Mpeg.h"

// The IPU's colour space conversion conforms to ITU-R Recommendation BT.601 if anyone wants to make a
// faster or "more accurate" implementation, but this is the precise documented integer method used by
// the hardware and is fast enough with SSE2.

#define IPU_Y_BIAS    16
#define IPU_C_BIAS    128
#define IPU_Y_COEFF   0x95	//  1.1640625
#define IPU_GCR_COEFF (-0x68)	// -0.8125
#define IPU_GCB_COEFF (-0x32)	// -0.390625
#define IPU_RCR_COEFF 0xcc	//  1.59375
#define IPU_BCB_COEFF 0x102	//  2.015625

MULTI_ISA_UNSHARED_START

// conforming implementation for reference, do not optimise
void yuv2rgb_reference(void)
{
	const macroblock_8& mb8 = decoder.mb8;
	macroblock_rgb32& rgb32 = decoder.rgb32;

	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
		{
			s32 lum = (IPU_Y_COEFF * (std::max(0, (s32)mb8.Y[y][x] - IPU_Y_BIAS))) >> 6;
			s32 rcr = (IPU_RCR_COEFF * ((s32)mb8.Cr[y>>1][x>>1] - 128)) >> 6;
			s32 gcr = (IPU_GCR_COEFF * ((s32)mb8.Cr[y>>1][x>>1] - 128)) >> 6;
			s32 gcb = (IPU_GCB_COEFF * ((s32)mb8.Cb[y>>1][x>>1] - 128)) >> 6;
			s32 bcb = (IPU_BCB_COEFF * ((s32)mb8.Cb[y>>1][x>>1] - 128)) >> 6;

			rgb32.c[y][x].r = std::max(0, std::min(255, (lum + rcr + 1) >> 1));
			rgb32.c[y][x].g = std::max(0, std::min(255, (lum + gcr + gcb + 1) >> 1));
			rgb32.c[y][x].b = std::max(0, std::min(255, (lum + bcb + 1) >> 1));
			rgb32.c[y][x].a = 0x80; // the norm to save doing this on the alpha pass
		}
}

#ifdef __GNUC__
typedef u16 u16x8 __attribute__((__vector_size__(16)));
typedef u8  u8x16 __attribute__((__vector_size__(16)));
constexpr static u16x8 splat_u16(u16 x) { return u16x8{x, x, x, x, x, x, x, x}; }
constexpr static u8x16 splat_u8(u8 x) { return u8x16{x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x}; }
constexpr static u32 splat_u16_u32(u16 x) { return static_cast<u32>(x) | (static_cast<u32>(x) << 16); }
constexpr static u32 splat_u8_u32(u8 x) { return splat_u16_u32(static_cast<u16>(x) | (static_cast<u16>(x) << 8)); }
#endif

// Suikoden Tactics FMV speed results: Reference - ~72fps, SSE2 - ~120fps
// An AVX2 version is only slightly faster than an SSE2 version (+2-3fps)
// (or I'm a poor optimiser), though it might be worth attempting again
// once we've ported to 64 bits (the extra registers should help).
__ri void yuv2rgb_sse2()
{
#ifdef __GNUC__
	static constexpr u8x16 shuffle = u8x16{0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15};
	static constexpr u8x16 c_bias = splat_u8(IPU_C_BIAS);
	static constexpr u8x16 y_bias = splat_u8(IPU_Y_BIAS);
	static constexpr u16x8 y_coefficient = splat_u16(IPU_Y_COEFF << 2);
	static constexpr u16x8 gcr_coefficient = splat_u16(static_cast<u16>(static_cast<u16>(IPU_GCR_COEFF) << 2));
	static constexpr u16x8 gcb_coefficient = splat_u16(static_cast<u16>(static_cast<u16>(IPU_GCB_COEFF) << 2));
	static constexpr u16x8 rcr_coefficient = splat_u16(static_cast<u16>(static_cast<u16>(IPU_RCR_COEFF) << 2));
	static constexpr u16x8 bcb_coefficient = splat_u16(static_cast<u16>(static_cast<u16>(IPU_BCB_COEFF) << 2));
	__m128i temp[11];
	sptr n = -64;
	void* output = &decoder.rgb32;
	asm volatile(R"(
		vpcmpeqd %[round_off], %[round_off], %[round_off]
		vpsllw   $8, %[round_off], %[y_mask]

		# The large number of unpacks at the end end up conflicting with the loading of initial values for the next iteration
		# This greatly reduces the amount of IPC we can get
		# Work around this by loading values at the end of the previous loop iteration
		# This will load a bit too far, but it won't go off the end of the large containing struct so whatever
		vmovq      320(%[input]), %[t0] # Load Initial Cr
		vmovq      256(%[input]), %[t1] # Load Initial Cb
		vpxor      %[t2], %[t2], %[t2]
		vpxor      %[t0], %[c_bias], %[t0]
		vpunpcklbw %[t0], %[t2], %[t0]
		vpxor      %[t1], %[c_bias], %[t1]
		vpunpcklbw %[t1], %[t2], %[t1]
	.balign 16
	0:
		vpmulhw    %[rcr_coeff], %[t0], %[t2]
		vpmulhw    %[gcr_coeff], %[t0], %[t3]
		vpmulhw    %[gcb_coeff], %[t1], %[t5]
		vpmulhw    %[bcb_coeff], %[t1], %[t4]
		vpaddw     %[t2], %[t5], %[t2]
		vpsubw     %[round_off], %[t2], %[t2]
		vpsubw     %[round_off], %[t3], %[t3]
		vpsubw     %[round_off], %[t4], %[t4]

		vmovdqa    256(%[input], %[n], 4), %[t1] # Load Y (Row 0)
		vpsubusb   %[y_bias], %[t1], %[t1]
		vpsllw     $8, %[t1], %[t0]
		vpmulhuw   %[y_coeff], %[t0], %[t0]
		vpand      %[t1], %[y_mask], %[t1]
		vpmulhuw   %[y_coeff], %[t1], %[t1]

		vpaddw     %[t2], %[t0], %[t5]
		vpaddw     %[t2], %[t1], %[t6]
		vpsraw     $1, %[t5], %[t5]
		vpsraw     $1, %[t6], %[t6]
		vpackuswb  %[t6], %[t5], %[t5]
		vpshufb    %[shuffle], %[t5], %[t5]
		vpaddw     %[t3], %[t0], %[t6]
		vpaddw     %[t3], %[t1], %[t7]
		vpsraw     $1, %[t6], %[t6]
		vpsraw     $1, %[t7], %[t7]
		vpackuswb  %[t7], %[t6], %[t6]
		vpshufb    %[shuffle], %[t6], %[t6]
		vpaddw     %[t4], %[t0], %[t7]
		vpaddw     %[t4], %[t1], %[t1]
		vpsraw     $1, %[t7], %[t7]
		vpsraw     $1, %[t1], %[t1]
		vpackuswb  %[t1], %[t7], %[t7]
		vpshufb    %[shuffle], %[t7], %[t7]

		vpunpcklbw %[t6], %[t5], %[t0]
		vpunpcklbw %[c_bias], %[t7], %[t1]
		vpunpcklwd %[t1], %[t0], %[t8]
		vmovdqa    %[t8], 0x00(%[output])
		vpunpckhwd %[t1], %[t0], %[t8]
		vmovdqa    %[t8], 0x10(%[output])

		vpunpckhbw %[t6], %[t5], %[t0]
		vpunpckhbw %[c_bias], %[t7], %[t1]
		vpunpcklwd %[t1], %[t0], %[t8]
		vmovdqa    %[t8], 0x20(%[output])
		vpunpckhwd %[t1], %[t0], %[t8]
		vmovdqa    %[t8], 0x30(%[output])

		vmovdqa    272(%[input], %[n], 4), %[t1] # Load Y (Row 1)
		vpsubusb   %[y_bias], %[t1], %[t1]
		vpsllw     $8, %[t1], %[t0]
		vpmulhuw   %[y_coeff], %[t0], %[t0]
		vpand      %[t1], %[y_mask], %[t1]
		vpmulhuw   %[y_coeff], %[t1], %[t1]

		vpaddw     %[t2], %[t0], %[t5]
		vpaddw     %[t2], %[t1], %[t6]
		vpsraw     $1, %[t5], %[t5]
		vpsraw     $1, %[t6], %[t6]
		vpackuswb  %[t6], %[t5], %[t5]
		vpaddw     %[t3], %[t0], %[t6]
		vpaddw     %[t3], %[t1], %[t7]
		vpsraw     $1, %[t6], %[t6]
		vpsraw     $1, %[t7], %[t7]
		vpackuswb  %[t7], %[t6], %[t6]
		vpaddw     %[t4], %[t0], %[t7]
		vpaddw     %[t4], %[t1], %[t1]
		vpsraw     $1, %[t7], %[t7]
		vpsraw     $1, %[t1], %[t1]
		vpackuswb  %[t1], %[t7], %[t7]

		vmovq      392(%[input], %[n]), %[t0] # Load Next Cr
		vmovq      328(%[input], %[n]), %[t1] # Load Next Cb
		vpxor      %[t2], %[t2], %[t2]
		vpxor      %[t0], %[c_bias], %[t0]
		vpunpcklbw %[t0], %[t2], %[t0]
		vpxor      %[t1], %[c_bias], %[t1]
		vpunpcklbw %[t1], %[t2], %[t1]

		vpshufb    %[shuffle], %[t5], %[t5]
		vpshufb    %[shuffle], %[t6], %[t6]
		vpshufb    %[shuffle], %[t7], %[t7]

		vpunpcklbw %[t6], %[t5], %[t2]
		vpunpcklbw %[c_bias], %[t7], %[t3]
		vpunpcklwd %[t3], %[t2], %[t4]
		vmovdqa    %[t4], 0x40(%[output])
		vpunpckhwd %[t3], %[t2], %[t4]
		vmovdqa    %[t4], 0x50(%[output])

		vpunpckhbw %[t6], %[t5], %[t2]
		vpunpckhbw %[c_bias], %[t7], %[t3]
		vpunpcklwd %[t3], %[t2], %[t4]
		vmovdqa    %[t4], 0x60(%[output])
		vpunpckhwd %[t3], %[t2], %[t4]
		vmovdqa    %[t4], 0x70(%[output])

		subq $-128, %[output]
		addq $8, %[n]
		jne 0b
	)"
		: [t0]"=&x"(temp[0]), [t1]"=&x"(temp[1]), [t2]"=&x"(temp[2]), [t3]"=&x"(temp[3]),
		  [t4]"=&x"(temp[4]), [t5]"=&x"(temp[5]), [t6]"=&x"(temp[6]), [t7]"=&x"(temp[7]),
		  [t8]"=&x"(temp[8]), [round_off]"=&x"(temp[9]), [y_mask]"=&x"(temp[10]),
		  [n]"+r"(n), [output]"+r"(output)
		: [c_bias]"x"(c_bias), [y_bias]"x"(y_bias),
		  [y_coeff]"x"(y_coefficient), [shuffle]"x"(shuffle),
		  [gcr_coeff]"m"(gcr_coefficient), [gcb_coeff]"m"(gcb_coefficient),
		  [rcr_coeff]"m"(rcr_coefficient), [bcb_coeff]"m"(bcb_coefficient),
		  [input]"r"(&decoder.mb8)
		: "memory");
#else
	const __m128i c_bias = _mm_set1_epi8(s8(IPU_C_BIAS));
	const __m128i y_bias = _mm_set1_epi8(IPU_Y_BIAS);
	const __m128i y_mask = _mm_set1_epi16(s16(0xFF00));
	// Specifying round off instead of round down as everywhere else
	// implies that this is right
	const __m128i round_1bit = _mm_set1_epi16(0x0001);

	const __m128i y_coefficient = _mm_set1_epi16(s16(IPU_Y_COEFF << 2));
	const __m128i gcr_coefficient = _mm_set1_epi16(s16(u16(IPU_GCR_COEFF) << 2));
	const __m128i gcb_coefficient = _mm_set1_epi16(s16(u16(IPU_GCB_COEFF) << 2));
	const __m128i rcr_coefficient = _mm_set1_epi16(s16(IPU_RCR_COEFF << 2));
	const __m128i bcb_coefficient = _mm_set1_epi16(s16(IPU_BCB_COEFF << 2));

	// Alpha set to 0x80 here. The threshold stuff is done later.
	const __m128i& alpha = c_bias;

	for (int n = 0; n < 8; ++n) {
		// could skip the loadl_epi64 but most SSE instructions require 128-bit
		// alignment so two versions would be needed.
		__m128i cb = _mm_loadl_epi64(reinterpret_cast<__m128i*>(&decoder.mb8.Cb[n][0]));
		__m128i cr = _mm_loadl_epi64(reinterpret_cast<__m128i*>(&decoder.mb8.Cr[n][0]));

		// (Cb - 128) << 8, (Cr - 128) << 8
		cb = _mm_xor_si128(cb, c_bias);
		cr = _mm_xor_si128(cr, c_bias);
		cb = _mm_unpacklo_epi8(_mm_setzero_si128(), cb);
		cr = _mm_unpacklo_epi8(_mm_setzero_si128(), cr);

		__m128i rc = _mm_mulhi_epi16(cr, rcr_coefficient);
		__m128i gc = _mm_adds_epi16(_mm_mulhi_epi16(cr, gcr_coefficient), _mm_mulhi_epi16(cb, gcb_coefficient));
		__m128i bc = _mm_mulhi_epi16(cb, bcb_coefficient);

		for (int m = 0; m < 2; ++m) {
			__m128i y = _mm_load_si128(reinterpret_cast<__m128i*>(&decoder.mb8.Y[n * 2 + m][0]));
			y = _mm_subs_epu8(y, y_bias);
			// Y << 8 for pixels 0, 2, 4, 6, 8, 10, 12, 14
			__m128i y_even = _mm_slli_epi16(y, 8);
			// Y << 8 for pixels 1, 3, 5, 7 ,9, 11, 13, 15
			__m128i y_odd = _mm_and_si128(y, y_mask);

			y_even = _mm_mulhi_epu16(y_even, y_coefficient);
			y_odd  = _mm_mulhi_epu16(y_odd,  y_coefficient);

			__m128i r_even = _mm_adds_epi16(rc, y_even);
			__m128i r_odd  = _mm_adds_epi16(rc, y_odd);
			__m128i g_even = _mm_adds_epi16(gc, y_even);
			__m128i g_odd  = _mm_adds_epi16(gc, y_odd);
			__m128i b_even = _mm_adds_epi16(bc, y_even);
			__m128i b_odd  = _mm_adds_epi16(bc, y_odd);

			// round
			r_even = _mm_srai_epi16(_mm_add_epi16(r_even, round_1bit), 1);
			r_odd  = _mm_srai_epi16(_mm_add_epi16(r_odd,  round_1bit), 1);
			g_even = _mm_srai_epi16(_mm_add_epi16(g_even, round_1bit), 1);
			g_odd  = _mm_srai_epi16(_mm_add_epi16(g_odd,  round_1bit), 1);
			b_even = _mm_srai_epi16(_mm_add_epi16(b_even, round_1bit), 1);
			b_odd  = _mm_srai_epi16(_mm_add_epi16(b_odd,  round_1bit), 1);

			// combine even and odd bytes in original order
			__m128i r = _mm_packus_epi16(r_even, r_odd);
			__m128i g = _mm_packus_epi16(g_even, g_odd);
			__m128i b = _mm_packus_epi16(b_even, b_odd);

			r = _mm_unpacklo_epi8(r, _mm_shuffle_epi32(r, _MM_SHUFFLE(3, 2, 3, 2)));
			g = _mm_unpacklo_epi8(g, _mm_shuffle_epi32(g, _MM_SHUFFLE(3, 2, 3, 2)));
			b = _mm_unpacklo_epi8(b, _mm_shuffle_epi32(b, _MM_SHUFFLE(3, 2, 3, 2)));

			// Create RGBA (we could generate A here, but we don't) quads
			__m128i rg_l = _mm_unpacklo_epi8(r, g);
			__m128i ba_l = _mm_unpacklo_epi8(b, alpha);
			__m128i rgba_ll = _mm_unpacklo_epi16(rg_l, ba_l);
			__m128i rgba_lh = _mm_unpackhi_epi16(rg_l, ba_l);

			__m128i rg_h = _mm_unpackhi_epi8(r, g);
			__m128i ba_h = _mm_unpackhi_epi8(b, alpha);
			__m128i rgba_hl = _mm_unpacklo_epi16(rg_h, ba_h);
			__m128i rgba_hh = _mm_unpackhi_epi16(rg_h, ba_h);

			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][0]), rgba_ll);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][4]), rgba_lh);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][8]), rgba_hl);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][12]), rgba_hh);
		}
	}
#endif
}

MULTI_ISA_UNSHARED_END
