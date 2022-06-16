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
#include "GSVertexTrace.h"
#include "GS/GSUtil.h"
#include "GS/GSState.h"
#include <cfloat>

CONSTINIT const GSVector4 GSVertexTrace::s_minmax = GSVector4::cxpr(FLT_MAX, -FLT_MAX, 0.f, 0.f);

GSVertexTrace::GSVertexTrace(const GSState* state, bool provoking_vertex_first)
	: m_accurate_stq(false), m_state(state), m_primclass(GS_INVALID_CLASS)
{
	memset(&m_alpha, 0, sizeof(m_alpha));

	#define InitUpdate3(P, IIP, TME, FST, COLOR) \
	m_fmm[COLOR][FST][TME][IIP][P] = \
		provoking_vertex_first ? &GSVertexTrace::FindMinMax<P, IIP, TME, FST, COLOR, true> : \
                                 &GSVertexTrace::FindMinMax<P, IIP, TME, FST, COLOR, false>;

	#define InitUpdate2(P, IIP, TME) \
		InitUpdate3(P, IIP, TME, 0, 0) \
		InitUpdate3(P, IIP, TME, 0, 1) \
		InitUpdate3(P, IIP, TME, 1, 0) \
		InitUpdate3(P, IIP, TME, 1, 1) \

	#define InitUpdate(P) \
		InitUpdate2(P, 0, 0) \
		InitUpdate2(P, 0, 1) \
		InitUpdate2(P, 1, 0) \
		InitUpdate2(P, 1, 1) \

	InitUpdate(GS_POINT_CLASS);
	InitUpdate(GS_LINE_CLASS);
	InitUpdate(GS_TRIANGLE_CLASS);
	InitUpdate(GS_SPRITE_CLASS);

	#define InitFMMRS(tme, fst) \
		m_fmm_round_sprite[tme][fst][0] = &GSVertexTrace::FMMRoundSprite<tme, fst, 0>; \
		m_fmm_round_sprite[tme][fst][1] = &GSVertexTrace::FMMRoundSprite<tme, fst, 1>;

	InitFMMRS(0, 0)
	InitFMMRS(0, 1)
	InitFMMRS(1, 0)
	InitFMMRS(1, 1)

	#define InitTri2Sprite3(IIP, TME, FST, COLOR, Z, FGE) \
		m_tri2sprite[IIP][TME][FST][COLOR][Z][FGE] = \
			provoking_vertex_first ? Tri2Sprite<IIP, TME, FST, COLOR, Z, FGE, true> \
			                       : Tri2Sprite<IIP, TME, FST, COLOR, Z, FGE, false>;

	#define InitTri2Sprite2(IIP, TME, FST, COLOR) \
		InitTri2Sprite3(IIP, TME, FST, COLOR, 0, 0) \
		InitTri2Sprite3(IIP, TME, FST, COLOR, 0, 1) \
		InitTri2Sprite3(IIP, TME, FST, COLOR, 1, 0) \
		InitTri2Sprite3(IIP, TME, FST, COLOR, 1, 1) \

	#define InitTri2Sprite(IIP, TME) \
		InitTri2Sprite2(IIP, TME, 0, 0) \
		InitTri2Sprite2(IIP, TME, 0, 1) \
		InitTri2Sprite2(IIP, TME, 1, 0) \
		InitTri2Sprite2(IIP, TME, 1, 1) \

	InitTri2Sprite(0, 0)
	InitTri2Sprite(0, 1)
	InitTri2Sprite(1, 0)
	InitTri2Sprite(1, 1)
}

bool GSVertexTrace::Tri2Sprite(GSVertex* RESTRICT vout, const GSVertex* RESTRICT vin, const u32* RESTRICT index, int nindex)
{
	const GSDrawingContext* context = m_state->m_context;
	const GIFRegPRIM* PRIM = m_state->PRIM;
	u32 iip = PRIM->IIP;
	u32 tme = PRIM->TME;
	u32 fst = PRIM->FST;
	u32 color = !(PRIM->TME && context->TEX0.TFX == TFX_DECAL && context->TEX0.TCC);
	u32 fge = PRIM->FGE;
	u32 z = context->TEST.ZTST == ZTST_GEQUAL || context->TEST.ZTST == ZTST_GREATER || !context->ZBUF.ZMSK;
	return m_tri2sprite[iip][tme][fst][color][z][fge](vout, vin, index, nindex);
}

void GSVertexTrace::UpdateRoundSprite(void* vertex, int count)
{
	m_primclass = GS_SPRITE_CLASS;
	u32 tme = m_state->PRIM->TME;
	u32 fst = m_state->PRIM->FST;
	u32 color = !(m_state->PRIM->TME && m_state->m_context->TEX0.TFX == TFX_DECAL && m_state->m_context->TEX0.TCC);
	(this->*m_fmm_round_sprite[tme][fst][color])(vertex, count);
	FinishUpdate(fst, vertex, count);
}

void GSVertexTrace::Update(const void* vertex, const u32* index, int v_count, int i_count, GS_PRIM_CLASS primclass)
{
	if (i_count == 0)
		return;

	m_primclass = primclass;

	u32 iip = m_state->PRIM->IIP;
	u32 tme = m_state->PRIM->TME;
	u32 fst = m_state->PRIM->FST;
	u32 color = !(m_state->PRIM->TME && m_state->m_context->TEX0.TFX == TFX_DECAL && m_state->m_context->TEX0.TCC);

	(this->*m_fmm[color][fst][tme][iip][primclass])(vertex, index, i_count);

	FinishUpdate(fst, vertex, v_count);
}

void GSVertexTrace::FinishUpdate(bool fst, const void* vertex, int count)
{
	// Potential float overflow detected. Better uses the slower division instead
	// Note: If Q is too big, 1/Q will end up as 0. 1e30 is a random number
	// that feel big enough.
	if (!fst && !m_accurate_stq && m_min.t.z > 1e30)
	{
		fprintf(stderr, "Vertex Trace: float overflow detected ! min %e max %e\n", m_min.t.z, m_max.t.z);
		m_accurate_stq = true;
	}

	m_eq.value = (m_min.c == m_max.c).mask() | ((m_min.p == m_max.p).mask() << 16) | ((m_min.t == m_max.t).mask() << 20);

	m_alpha.valid = false;

	// I'm not sure of the cost. In doubt let's do it only when depth is enabled
	if (m_state->m_context->TEST.ZTE == 1 && m_state->m_context->TEST.ZTST > ZTST_ALWAYS)
	{
		CorrectDepthTrace(vertex, count);
	}

	if (m_state->PRIM->TME)
	{
		const GIFRegTEX1& TEX1 = m_state->m_context->TEX1;

		m_filter.mmag = TEX1.IsMagLinear();
		m_filter.mmin = TEX1.IsMinLinear();

		if (TEX1.MXL == 0) // MXL == 0 => MMIN ignored, tested it on ps2
		{
			m_filter.linear = m_filter.mmag;
		}
		else
		{
			float K = (float)TEX1.K / 16;

			if (TEX1.LCM == 0 && m_state->PRIM->FST == 0) // FST == 1 => Q is not interpolated
			{
				// LOD = log2(1/|Q|) * (1 << L) + K

				GSVector4::storel(&m_lod, m_max.t.uph(m_min.t).log2(3).neg() * (float)(1 << TEX1.L) + K);

				if (m_lod.x > m_lod.y)
				{
					float tmp = m_lod.x;
					m_lod.x = m_lod.y;
					m_lod.y = tmp;
				}
			}
			else
			{
				m_lod.x = K;
				m_lod.y = K;
			}

			if (m_lod.y <= 0)
			{
				m_filter.linear = m_filter.mmag;
			}
			else if (m_lod.x > 0)
			{
				m_filter.linear = m_filter.mmin;
			}
			else
			{
				m_filter.linear = m_filter.mmag | m_filter.mmin;
			}
		}

		switch (GSConfig.TextureFiltering)
		{
			case BiFiltering::Nearest:
				m_filter.opt_linear = 0;
				break;

			case BiFiltering::Forced_But_Sprite:
				// Special case to reduce the number of glitch when upscaling is enabled
				m_filter.opt_linear = (m_primclass == GS_SPRITE_CLASS) ? m_filter.linear : 1;
				break;

			case BiFiltering::Forced:
				m_filter.opt_linear = 1;
				break;

			case BiFiltering::PS2:
			default:
				m_filter.opt_linear = m_filter.linear;
				break;
		}
	}
}

template <GS_PRIM_CLASS primclass, u32 iip, u32 tme, u32 fst, u32 color, bool flat_swapped>
void GSVertexTrace::FindMinMax(const void* vertex, const u32* index, int count)
{
	const GSDrawingContext* context = m_state->m_context;

	int n = 1;

	switch (primclass)
	{
		case GS_POINT_CLASS:
			n = 1;
			break;
		case GS_LINE_CLASS:
		case GS_SPRITE_CLASS:
			n = 2;
			break;
		case GS_TRIANGLE_CLASS:
			n = 3;
			break;
	}

	GSVector4 tmin = s_minmax.xxxx();
	GSVector4 tmax = s_minmax.yyyy();
	GSVector4i cmin = GSVector4i::xffffffff();
	GSVector4i cmax = GSVector4i::zero();

	GSVector4i pmin = GSVector4i::xffffffff();
	GSVector4i pmax = GSVector4i::zero();

	const GSVertex* RESTRICT v = (GSVertex*)vertex;

	// Process 2 vertices at a time for increased efficiency
	auto processVertices = [&](const GSVertex& v0, const GSVertex& v1, bool finalVertex)
	{
		if (color)
		{
			GSVector4i c0 = GSVector4i::load(v0.RGBAQ.U32[0]);
			GSVector4i c1 = GSVector4i::load(v1.RGBAQ.U32[0]);
			if (iip || finalVertex)
			{
				cmin = cmin.min_u8(c0.min_u8(c1));
				cmax = cmax.max_u8(c0.max_u8(c1));
			}
			else if (n == 2)
			{
				// For even n, we process v1 and v2 of the same prim
				// (For odd n, we process one vertex from each of two prims)
				cmin = cmin.min_u8(c1);
				cmax = cmax.max_u8(c1);
			}
		}

		if (tme)
		{
			if (!fst)
			{
				GSVector4 stq0 = GSVector4::cast(GSVector4i(v0.m[0]));
				GSVector4 stq1 = GSVector4::cast(GSVector4i(v1.m[0]));

				GSVector4 q;
				// Sprites always have indices == vertices, so we don't have to look at the index table here
				if (primclass == GS_SPRITE_CLASS)
					q = stq1.wwww();
				else
					q = stq0.wwww(stq1);

				// Note: If in the future this is changed in a way that causes parts of calculations to go unused,
				//       make sure to remove the z (rgba) field as it's often denormal.
				//       Then, use GSVector4::noopt() to prevent clang from optimizing out your "useless" shuffle
				//       e.g. stq = (stq.xyww() / stq.wwww()).noopt().xyww(stq);
				GSVector4 st = stq0.xyxy(stq1) / q;

				stq0 = st.xyww(primclass == GS_SPRITE_CLASS ? stq1 : stq0);
				stq1 = st.zwww(stq1);

				tmin = tmin.min(stq0.min(stq1));
				tmax = tmax.max(stq0.max(stq1));
			}
			else
			{
				GSVector4i uv0(v0.m[1]);
				GSVector4i uv1(v1.m[1]);

				GSVector4 st0 = GSVector4(uv0.uph16()).xyxy();
				GSVector4 st1 = GSVector4(uv1.uph16()).xyxy();

				tmin = tmin.min(st0.min(st1));
				tmax = tmax.max(st0.max(st1));
			}
		}

		GSVector4i xyzf0(v0.m[1]);
		GSVector4i xyzf1(v1.m[1]);

		GSVector4i xy0 = xyzf0.upl16();
		GSVector4i z0 = xyzf0.yyyy();
		GSVector4i xy1 = xyzf1.upl16();
		GSVector4i z1 = xyzf1.yyyy();

		GSVector4i p0 = xy0.blend16<0xf0>(z0.uph32(primclass == GS_SPRITE_CLASS ? xyzf1 : xyzf0));
		GSVector4i p1 = xy1.blend16<0xf0>(z1.uph32(xyzf1));

		pmin = pmin.min_u32(p0.min_u32(p1));
		pmax = pmax.max_u32(p0.max_u32(p1));
	};

	if (n == 2)
	{
		for (int i = 0; i < count; i += 2)
		{
			processVertices(v[index[i + 0]], v[index[i + 1]], false);
		}
	}
	else if (iip || n == 1) // iip means final and non-final vertexes are treated the same
	{
		int i = 0;
		for (; i < (count - 1); i += 2) // 2x loop unroll
		{
			processVertices(v[index[i + 0]], v[index[i + 1]], true);
		}
		if (count & 1)
		{
			// Compiler optimizations go!
			// (And if they don't, it's only one vertex out of many)
			processVertices(v[index[i]], v[index[i]], true);
		}
	}
	else if (n == 3)
	{
		int i = 0;
		for (; i < (count - 3); i += 6)
		{
			processVertices(v[index[i + 0]], v[index[i + 3]], flat_swapped);
			processVertices(v[index[i + 1]], v[index[i + 4]], false);
			processVertices(v[index[i + 2]], v[index[i + 5]], !flat_swapped);
		}
		if (count & 1)
		{
			processVertices(v[index[i + 0]], v[index[i + 1]], flat_swapped);
			// Compiler optimizations go!
			// (And if they don't, it's only one vertex out of many)
			processVertices(v[index[i + 2]], v[index[i + 2]], !flat_swapped);
		}
	}
	else
	{
		pxAssertRel(0, "Bad n value");
	}

	GSVector4 o(context->XYOFFSET);
	GSVector4 s(1.0f / 16, 1.0f / 16, 2.0f, 1.0f);

	m_min.p = (GSVector4(pmin) - o) * s;
	m_max.p = (GSVector4(pmax) - o) * s;

	// Fix signed int conversion
	m_min.p = m_min.p.insert32<0, 2>(GSVector4::load((float)(u32)pmin.extract32<2>()));
	m_max.p = m_max.p.insert32<0, 2>(GSVector4::load((float)(u32)pmax.extract32<2>()));

	if (tme)
	{
		if (fst)
		{
			s = GSVector4(1.0f / 16, 1.0f).xxyy();
		}
		else
		{
			s = GSVector4(1 << context->TEX0.TW, 1 << context->TEX0.TH, 1, 1);
		}

		m_min.t = tmin * s;
		m_max.t = tmax * s;
	}
	else
	{
		m_min.t = GSVector4::zero();
		m_max.t = GSVector4::zero();
	}

	if (color)
	{
		m_min.c = cmin.u8to32();
		m_max.c = cmax.u8to32();
	}
	else
	{
		m_min.c = GSVector4i::zero();
		m_max.c = GSVector4i::zero();
	}
}

struct alignas(2) TriangleOrdering
{
	// Describes a right triangle laid out in one of the following orientations
	// b   c | c  b | a     |     a
	// a     |    a | b   c | c   b
	u8 a; // Same x as b
	// b: same x as a, same y as c.  Not stored because we never actually use it
	u8 c; // Same y as b
	TriangleOrdering() = default;
	constexpr TriangleOrdering(u8 a, u8 b, u8 c)
		: a(a), c(c) {}
};
struct ComparisonResult
{
	u8 value;
	u8 FinalCmp() const { return value & 3; }
	u8 FinalOrder() const { return value >> 2; }
	constexpr ComparisonResult(u8 final_cmp, u8 final_order)
		: value(final_cmp | (final_order << 2)) {}
};

static bool AreTrianglesRight(TriangleOrdering* out_triangle0, TriangleOrdering* out_triangle1,
                              const GSVertex* RESTRICT vin, const u32* RESTRICT index)
{
	static constexpr TriangleOrdering order_lut[6] =
	{
		TriangleOrdering(0, 1, 2),
		TriangleOrdering(0, 2, 1),
		TriangleOrdering(1, 0, 2),
		TriangleOrdering(1, 2, 0),
		TriangleOrdering(2, 0, 1),
		TriangleOrdering(2, 1, 0),
	};

	static constexpr ComparisonResult comparison_lut[16] =
	{
		ComparisonResult(0, 0), // 0000 => None equal, no sprite possible
		ComparisonResult(2, 0), // 0001 => x0 = x1, requires y1 = y2
		ComparisonResult(1, 5), // 0010 => y0 = y1, requires x1 = x2
		ComparisonResult(2, 0), // 0011 => x0 = x1, y0 = y1, (no area) requires x1 = x2 or y1 = y2
		ComparisonResult(2, 1), // 0100 => x0 = x2, requires y1 = y2
		ComparisonResult(2, 0), // 0101 => x0 = x1, x0 = x2, (no area) requires y1 = y2
		ComparisonResult(0, 4), // 0110 => y0 = y1, x0 = x2, requires nothing
		ComparisonResult(0, 4), // 0111 => x0 = y1, y0 = y1, x0 = x2, (no area) requires nothing
		ComparisonResult(1, 3), // 1000 => y0 = y2, requires x1 = x2
		ComparisonResult(0, 2), // 1001 => x0 = x1, y0 = y2, requires nothing
		ComparisonResult(1, 3), // 1010 => y0 = y1, y0 = y2, (no area) requires x1 = x2
		ComparisonResult(0, 2), // 1011 => x0 = x1, y0 = y1, y0 = y2, (unlikely) requires nothing
		ComparisonResult(2, 1), // 1100 => x0 = x2, y0 = y2, (no area) requires x1 = x2 or y1 = y2
		ComparisonResult(0, 2), // 1101 => x0 = x1, x0 = x2, y0 = y2, (no area) requires nothing
		ComparisonResult(0, 4), // 1110 => y0 = y1, x0 = x2, y0 = y2, (no area) requires nothing
		ComparisonResult(0, 2), // 1111 => x0 = x1, y0 = y1, x0 = x2, y0 = y2, (no area) requires nothing
	};

	GSVector4i xy0 = GSVector4i(vin[index[0]].m[1]).upl16(); // Triangle 0 vertex 0
	GSVector4i xy1 = GSVector4i(vin[index[1]].m[1]).upl16(); // Triangle 0 vertex 1
	GSVector4i xy2 = GSVector4i(vin[index[2]].m[1]).upl16(); // Triangle 0 vertex 2
	GSVector4i xy3 = GSVector4i(vin[index[3]].m[1]).upl16(); // Triangle 1 vertex 0
	GSVector4i xy4 = GSVector4i(vin[index[4]].m[1]).upl16(); // Triangle 1 vertex 1
	GSVector4i xy5 = GSVector4i(vin[index[5]].m[1]).upl16(); // Triangle 1 vertex 2

	int cmp0 = GSVector4::cast(xy0.xyxy().eq32(xy1.upl64(xy2))).mask();
	int cmp1 = GSVector4::cast(xy3.xyxy().eq32(xy4.upl64(xy5))).mask();
	int cmp2 = GSVector4::cast(xy1.upl64(xy4).eq32(xy2.upl64(xy5))).mask();
	if (!cmp0 || !cmp1) // Either triangle 0 or triangle 1 isn't a right triangle
		return false;
	ComparisonResult triangle0cmp = comparison_lut[cmp0];
	ComparisonResult triangle1cmp = comparison_lut[cmp1];
	int required_cmp2 = triangle0cmp.FinalCmp() | (triangle1cmp.FinalCmp() << 2);
	if ((cmp2 & required_cmp2) != required_cmp2)
		return false;
	// Both t0 and t1 are right triangles!
	*out_triangle0 = order_lut[triangle0cmp.FinalOrder()];
	*out_triangle1 = order_lut[triangle1cmp.FinalOrder()];
	return true;
}

template <u32 iip, u32 tme, u32 fst, u32 color, u32 z, u32 fge, bool provoking_vertex_first>
bool GSVertexTrace::Tri2Sprite(GSVertex* RESTRICT vout, const GSVertex* RESTRICT vin, const u32* RESTRICT index, int nindex)
{
	for (int i = 0; i < nindex; i += 6, vout += 2)
	{
		const u32* t0idx = index + i;
		const u32* t1idx = t0idx + 3;
		TriangleOrdering tri0, tri1;
		if (!AreTrianglesRight(&tri0, &tri1, vin, index + i))
			return false;

		const GSVertex& v0 = vin[t0idx[0]];
		const GSVertex& v1 = vin[t0idx[1]];
		const GSVertex& v2 = vin[t0idx[2]];
		const GSVertex& v3 = vin[t0idx[3]];
		const GSVertex& v4 = vin[t0idx[4]];
		const GSVertex& v5 = vin[t0idx[5]];
		const GSVertex& t0a = vin[t0idx[tri0.a]];
		const GSVertex& t0c = vin[t0idx[tri0.c]];
		const GSVertex& t1a = vin[t1idx[tri1.a]];
		const GSVertex& t1c = vin[t1idx[tri1.c]];
		const GSVertex& t0provoking = provoking_vertex_first ? v0 : v2;
		const GSVertex& t1provoking = provoking_vertex_first ? v3 : v5;

		GSVector4i ok = GSVector4i(-1);
		// Verify that t0 and t1 have all their other data matching
		GSVector4i xyzuvfmask_fge = GSVector4i(-1, -1, -1, 0);
		GSVector4i xyzuvfmask_z   = GSVector4i(-1, 0, -1, -1);
		GSVector4i stcqmask_color = GSVector4i(-1, -1, 0, -1);
		GSVector4i stcqmask_q     = GSVector4i(-1, -1, -1, 0);
		bool needs_q = tme && !fst;
		if ((iip && fge) || z)
		{
			// Sprites don't interpolate, so make sure everything's the same
			GSVector4i mask = (iip && fge ? xyzuvfmask_fge : GSVector4i(-1)) & (z ? xyzuvfmask_z : GSVector4i(-1));
			ok &= GSVector4i(v0.m[1]).eq8(GSVector4i(v1.m[1])) | mask;
			ok &= GSVector4i(v0.m[1]).eq8(GSVector4i(v2.m[1])) | mask;
			ok &= GSVector4i(v3.m[1]).eq8(GSVector4i(v4.m[1])) | mask;
			ok &= GSVector4i(v3.m[1]).eq8(GSVector4i(v5.m[1])) | mask;
		}
		if ((iip && color) || needs_q)
		{
			GSVector4i mask = (iip && color ? stcqmask_color : GSVector4i(-1)) & (needs_q ? stcqmask_q : GSVector4i(-1));
			ok &= GSVector4i(v0.m[0]).eq8(GSVector4i(v1.m[0])) | mask;
			ok &= GSVector4i(v0.m[0]).eq8(GSVector4i(v2.m[0])) | mask;
			ok &= GSVector4i(v3.m[0]).eq8(GSVector4i(v4.m[0])) | mask;
			ok &= GSVector4i(v3.m[0]).eq8(GSVector4i(v5.m[0])) | mask;
		}
		if (fge || z)
		{
			GSVector4i mask = (fge ? xyzuvfmask_fge : GSVector4i(-1)) & (z ? xyzuvfmask_z : GSVector4i(-1));
			ok &= GSVector4i(t0provoking.m[1]).eq8(GSVector4i(t1provoking.m[1])) | mask;
		}
		if (color || needs_q)
		{
			GSVector4i mask = (color ? stcqmask_color : GSVector4i(-1)) & (needs_q ? stcqmask_q : GSVector4i(-1));
			ok &= GSVector4i(t0provoking.m[0]).eq8(GSVector4i(t1provoking.m[0])) | mask;
		}

		// Need to verify that t0a == t1c and t0c == t1a
		if (tme)
		{
			if (fst)
			{
				// XY and UV
				ok &= GSVector4i(t0a.m[1]).eq8(GSVector4i(t1c.m[1])) | GSVector4i(0, -1, 0, -1);
				ok &= GSVector4i(t0c.m[1]).eq8(GSVector4i(t1a.m[1])) | GSVector4i(0, -1, 0, -1);
			}
			else
			{
				// XY and ST
				ok &= GSVector4i(t0a.m[1]).eq8(GSVector4i(t1c.m[1])) | GSVector4i(0, -1, -1, -1);
				ok &= GSVector4i(t0c.m[1]).eq8(GSVector4i(t1a.m[1])) | GSVector4i(0, -1, -1, -1);
				ok &= GSVector4i(t0a.m[0]).eq8(GSVector4i(t1c.m[0])) | GSVector4i(0, 0, -1, -1);
				ok &= GSVector4i(t0c.m[0]).eq8(GSVector4i(t1a.m[0])) | GSVector4i(0, 0, -1, -1);
			}
		}
		else
		{
			// XY only
			ok &= GSVector4i(t0a.m[1]).eq8(GSVector4i(t1c.m[1])) | GSVector4i(0, -1, -1, -1);
			ok &= GSVector4i(t0c.m[1]).eq8(GSVector4i(t1a.m[1])) | GSVector4i(0, -1, -1, -1);
		}

		if (!ok.alltrue())
			return false;

		vout[0].m[0] = t0a.m[0];
		vout[0].m[1] = t0a.m[1];
		vout[1].m[0] = t0c.m[0];
		vout[1].m[1] = t0c.m[1];
		if (!iip)
		{
			// Need to copy provoking vertex values
			if (color)
				vout[1].RGBAQ.U32[0] = t0provoking.RGBAQ.U32[0];
			if (fge)
				vout[1].FOG = t0provoking.FOG;
		}
	}
	return true;
}

template <u32 tme, u32 fst, u32 color>
void GSVertexTrace::FMMRoundSprite(void* vertex, int count)
{
	const GSDrawingContext* context = m_state->m_context;

	GSVector4 tmin = s_minmax.xxxx();
	GSVector4 tmax = s_minmax.yyyy();
	GSVector4i cmin = GSVector4i::xffffffff();
	GSVector4i cmax = GSVector4i::zero();

	GSVector4i pmin = GSVector4i::xffffffff();
	GSVector4i pmax = GSVector4i::zero();

	GSVertex* RESTRICT v = (GSVertex*)vertex;

	ASSERT(count % 2 == 0);

	const GIFRegTEX0& TEX0 = context->TEX0;
	bool adjust_texture = GSConfig.UserHacks_RoundSprite == 2;

	// A PS2 a sprite starting at (0, 0) draws its first point with the equivalent location of (0, 0)
	// On PC, the first point is in the center, at (0.5, 0.5)
	// Adjust st by (-0.5, -0.5) * dst/dxy to compensate
	// (If nearest, assume the game meant exactly those coordinates)
	GSVector4i st_adjust_base = adjust_texture ? GSVector4i(-8) : GSVector4i::zero();
	GSVector4i minmax_adjust_enable = adjust_texture ? GSVector4i(-1) : GSVector4i::zero();
	// Multiplier for converting uv coordinates to st
	GSVector4 uv_multiplier = GSVector4(1.f/16.f) / GSVector4(1 << TEX0.TW, 1 << TEX0.TH).xyxy();

	for (int i = 0; i < count; i += 2)
	{
		GSVertex& v0 = v[i];
		GSVertex& v1 = v[i + 1];

		if (color)
		{
			GSVector4i c1 = GSVector4i::load(v1.RGBAQ.U32[0]);
			cmin = cmin.min_u8(c1);
			cmax = cmax.max_u8(c1);
		}

		GSVector4i xyzf0(v0.m[1]);
		GSVector4i xyzf1(v1.m[1]);

		GSVector4i xy0 = xyzf0.upl16();
		GSVector4i z0 = xyzf0.yyyy();
		GSVector4i xy1 = xyzf1.upl16();
		GSVector4i z1 = xyzf1.yyyy();

		GSVector4i xydiff = xy1 - xy0;
		GSVector4i xy = xy0.upl64(xy1);
		GSVector4i xyoffset = GSVector4i::loadl(&context->XYOFFSET.U64).xyxy();
		// Round everything up to the nearest whole number
		// (Since these are the actual positions pixels will be drawn if rendering at native resolution)
		GSVector4i xy_adjusted = ((xy - xyoffset + 0xf) & ~0xf) + xyoffset;
		// The amount the point was moved by
		GSVector4i xy_adjust_amt = xy_adjusted - xy;
		// Store back the adjusted xy coordinates
		GSVector4i xy_adjusted_packed = xy_adjusted.pu32();
		v0.XYZ.U32[0] = xy_adjusted_packed.extract32<0>();
		v1.XYZ.U32[0] = xy_adjusted_packed.extract32<1>();

		GSVector4i p0 = xy_adjusted.blend16<0xf0>(z0.uph32(xyzf1));
		GSVector4i p1 = xy_adjusted.uph64(z1.uph32(xyzf1));
		pmin = pmin.min_u32(p0.min_u32(p1));
		pmax = pmax.max_u32(p0.max_u32(p1));

		if (tme)
		{
			GSVector4 st;
			GSVector4 stq0 = GSVector4::cast(GSVector4i(v0.m[0]));
			GSVector4 stq1 = GSVector4::cast(GSVector4i(v1.m[0]));
			if (!fst)
			{
				GSVector4 q = stq1.wwww();
				st = stq0.upld(stq1) / q;
			}
			else
			{
				GSVector4i uv0(v0.m[1]);
				GSVector4i uv1(v1.m[1]);
				GSVector4 st0 = GSVector4(uv0.uph16());
				GSVector4 st1 = GSVector4(uv1.uph16());

				st = st0.upld(st1);
			}
			GSVector4 stdiff = st.zwzw() - st.xyxy();
			GSVector4 dst_dxy = stdiff / GSVector4(xydiff.xyxy());
			// If we shifted the xy right by 0.25 pixels, we should shift st by 0.25 * dst/dxy to compensate
			GSVector4 st_adjusted = st + GSVector4(st_adjust_base + xy_adjust_amt) * dst_dxy;
			// While we're at it, we can shrink the bottom right of the minmax in since we know where the pixel centers will be
			GSVector4i is_bottom_right = xy_adjusted > xy_adjusted.zwxy();
			GSVector4i minmax_adjust_base = GSVector4i(-16) & is_bottom_right & minmax_adjust_enable; // top left => 0, bottom right => -16
			GSVector4 minmax = st + GSVector4(minmax_adjust_base + xy_adjust_amt) * dst_dxy;
			tmin = tmin.min(minmax.xyxy().min(minmax.zwzw()));
			tmax = tmax.max(minmax.xyxy().max(minmax.zwzw()));

			if (fst)
			{
				// We can't store back into uv because the value can go negative
				// See https://github.com/PCSX2/pcsx2/files/7012926/drakengard.2.decaling.zip for an example
				st_adjusted *= uv_multiplier;
			}

			GSVector4 rgbaq = stq1.uph(GSVector4(1.0f)); // [rgba, 1.0, oldq, 1.0], we want to replace q with 1 since we already divided by it
			v0.m[0] = GSVector4i::cast(st_adjusted.xyxy(rgbaq));
			v1.m[0] = GSVector4i::cast(st_adjusted.zwxy(rgbaq));
		}
	}

	GSVector4 o(context->XYOFFSET);
	GSVector4 s(1.0f / 16, 1.0f / 16, 2.0f, 1.0f);

	m_min.p = (GSVector4(pmin) - o) * s;
	m_max.p = (GSVector4(pmax) - o) * s;

	// Fix signed int conversion
	m_min.p = m_min.p.insert32<0, 2>(GSVector4::load((float)(u32)pmin.extract32<2>()));
	m_max.p = m_max.p.insert32<0, 2>(GSVector4::load((float)(u32)pmax.extract32<2>()));

	if (tme)
	{
		if (fst)
		{
			s = GSVector4(1.0f / 16, 1.0f).xxyy();
		}
		else
		{
			s = GSVector4(1 << TEX0.TW, 1 << TEX0.TH, 1, 1);
		}

		m_min.t = (tmin * s).upld(GSVector4(1));
		m_max.t = (tmax * s).upld(GSVector4(1));
	}
	else
	{
		m_min.t = GSVector4::zero();
		m_max.t = GSVector4::zero();
	}

	if (color)
	{
		m_min.c = cmin.u8to32();
		m_max.c = cmax.u8to32();
	}
	else
	{
		m_min.c = GSVector4i::zero();
		m_max.c = GSVector4i::zero();
	}
}

void GSVertexTrace::CorrectDepthTrace(const void* vertex, int count)
{
	if (m_eq.z == 0)
		return;

	// FindMinMax isn't accurate for the depth value. Lsb bit is always 0.
	// The code below will check that depth value is really constant
	// and will update m_min/m_max/m_eq accordingly
	//
	// Really impact Xenosaga3
	//
	// Hopefully function is barely called so AVX/SSE will be useless here


	const GSVertex* RESTRICT v = (GSVertex*)vertex;
	u32 z = v[0].XYZ.Z;

	// ought to check only 1/2 for sprite
	if (z & 1)
	{
		// Check that first bit is always 1
		for (int i = 0; i < count; i++)
		{
			z &= v[i].XYZ.Z;
		}
	}
	else
	{
		// Check that first bit is always 0
		for (int i = 0; i < count; i++)
		{
			z |= v[i].XYZ.Z;
		}
	}

	if (z == v[0].XYZ.Z)
	{
		m_eq.z = 1;
	}
	else
	{
		m_eq.z = 0;
	}
}
