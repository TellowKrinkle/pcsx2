/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022 PCSX2 Dev Team
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
#include <gtest/gtest.h>
#include "Renderers/Common/GSVertexTrace.h"
#include "GSState.h"

struct VTTest : GSVertexTrace
{
	GSDrawingContext context;
public:
	VTTest(VTTest&&) = delete;
	VTTest()
		// Not a real GSState but it's good enough for us
		: GSVertexTrace(static_cast<GSState*>(malloc(sizeof(GSState))), false)
	{
		const_cast<GSState*>(m_state)->m_context = &context;
		GSConfig.UserHacks_RoundSprite = 2;
		context.XYOFFSET.OFX = 1024;
		context.XYOFFSET.OFY = 1024;
		context.TEX1.MXL = 0;
		context.TEX1.MMAG = 1;
		// Allow us to use st like uv coordinates
		context.TEX0.TW = 0;
		context.TEX0.TH = 0;
	}
	~VTTest() { free(const_cast<void*>(static_cast<const void*>(m_state))); }

	void test(GSVertex v0, GSVertex v1, GSVertex out_v0, GSVertex out_v1, GSVector4 minmax)
	{
		std::string test_name = fmt::format("XY ({}, {}) -> ({}, {}), ST ({}, {}) -> {}, {})",
		                                    v0.XYZ.X, v0.XYZ.Y, v1.XYZ.X, v1.XYZ.Y,
		                                    v0.ST.S,  v0.ST.T,  v1.ST.S,  v1.ST.T);
		GSVertex arr[] = { v0, v1 };
		(this->*m_fmm_round_sprite[1][0][0])(arr, 2);

		EXPECT_EQ(arr[0].XYZ.X, out_v0.XYZ.X) << "V0.X (" << test_name << ")";
		EXPECT_EQ(arr[0].XYZ.Y, out_v0.XYZ.Y) << "V0.Y (" << test_name << ")";
		EXPECT_EQ(arr[0].ST.S,  out_v0.ST.S)  << "V0.S (" << test_name << ")";
		EXPECT_EQ(arr[0].ST.T,  out_v0.ST.T)  << "V0.T (" << test_name << ")";
		EXPECT_EQ(arr[1].XYZ.X, out_v1.XYZ.X) << "V1.X (" << test_name << ")";
		EXPECT_EQ(arr[1].XYZ.Y, out_v1.XYZ.Y) << "V1.Y (" << test_name << ")";
		EXPECT_EQ(arr[1].ST.S,  out_v1.ST.S)  << "V1.S (" << test_name << ")";
		EXPECT_EQ(arr[1].ST.T,  out_v1.ST.T)  << "V1.T (" << test_name << ")";
		EXPECT_EQ(m_min.t.x,    minmax.x)     << "MIN.S (" << test_name << ")";
		EXPECT_EQ(m_min.t.y,    minmax.y)     << "MIN.T (" << test_name << ")";
		EXPECT_EQ(m_max.t.x,    minmax.z)     << "MAX.S (" << test_name << ")";
		EXPECT_EQ(m_max.t.y,    minmax.w)     << "MAX.T (" << test_name << ")";
	}

	void test(GSVector4i in_xy, GSVector4 in_st, GSVector4i out_xy, GSVector4 out_st, GSVector4 minmax)
	{
		GSVertex v0, v1, out_v0, out_v1;
		v0.XYZ.X = in_xy.x;
		v0.XYZ.Y = in_xy.y;
		v1.XYZ.X = in_xy.z;
		v1.XYZ.Y = in_xy.w;
		v0.ST.S = in_st.x;
		v0.ST.T = in_st.y;
		v1.ST.S = in_st.z;
		v1.ST.T = in_st.w;
		v0.RGBAQ.Q = 1.0;
		v1.RGBAQ.Q = 1.0;
		out_v0.XYZ.X = out_xy.x;
		out_v0.XYZ.Y = out_xy.y;
		out_v1.XYZ.X = out_xy.z;
		out_v1.XYZ.Y = out_xy.w;
		out_v0.ST.S = out_st.x;
		out_v0.ST.T = out_st.y;
		out_v1.ST.S = out_st.z;
		out_v1.ST.T = out_st.w;
		test(v0, v1, out_v0, out_v1, minmax);
	}
};

TEST(CopyTest, Copy)
{
	VTTest tester;
	// Standard 64x64 square
	// Input xy 0 to 64, while st are 0.5 to 64.5
	tester.test(GSVector4i(0, 0, 1024, 1024), GSVector4(0.5f, 0.5f, 64.5f, 64.5f),
	            GSVector4i(0, 0, 1024, 1024), GSVector4(0.0f, 0.0f, 64.0f, 64.0f),
	                                          GSVector4(0.5f, 0.5f, 63.5f, 63.5f));
	// Weird 64x64 square, used by Shadow of Rome (see https://github.com/PCSX2/pcsx2/issues/5851)
	// Input xy -0.5 to -63.5, st are 0 to 64
	// GS coordinates are unsigned, so we use 0.5 to 64.5 to target a 1 to 65 square
	tester.test(GSVector4i( 8,  8, 1032, 1032), GSVector4(1.0f, 1.0f, 65.0f, 65.0f),
	            GSVector4i(16, 16, 1040, 1040), GSVector4(1.0f, 1.0f, 65.0f, 65.0f),
	                                            GSVector4(1.5f, 1.5f, 64.5f, 64.5f));
	// Extra weird 64x64 square, to make sure wierd things work
	tester.test(GSVector4i( 1, 12, 1025, 1036), GSVector4((9.f / 16), (20.f / 16), 64 + (9.f / 16), 64 + (20.f / 16)),
	            GSVector4i(16, 16, 1040, 1040), GSVector4(1.0f, 1.0f, 65.0f, 65.0f),
	                                            GSVector4(1.5f, 1.5f, 64.5f, 64.5f));
	// Backwards square.  Make sure everything works even when the coordinates are backwards
	tester.test(GSVector4i(1024, 1024, 0, 0), GSVector4(64.5f, 64.5f, 0.5f, 0.5f),
	            GSVector4i(1024, 1024, 0, 0), GSVector4(64.0f, 64.0f, 0.0f, 0.0f),
	                                          GSVector4(0.5f, 0.5f, 63.5f, 63.5f));
	// Flip.  Technically not a copy but whatever
	tester.test(GSVector4i(0, 0, 1024, 1024), GSVector4(64.5f, 64.5f, 0.5f, 0.5f),
	            GSVector4i(0, 0, 1024, 1024), GSVector4(65.0f, 65.0f, 1.0f, 1.0f),
	                                          GSVector4(1.5f, 1.5f, 64.5f, 64.5f));
	// Other flip
	tester.test(GSVector4i(1024, 1024, 0, 0), GSVector4(0.5f, 0.5f, 64.5f, 64.5f),
	            GSVector4i(1024, 1024, 0, 0), GSVector4(1.0f, 1.0f, 65.0f, 65.0f),
	                                          GSVector4(1.5f, 1.5f, 64.5f, 64.5f));
}

TEST(GrowTest, Grow)
{
	VTTest tester;
	// Standard upscale
	// Input xy 0 to 64, while st are 0.25 to 32.25
	tester.test(GSVector4i(0, 0, 1024, 1024), GSVector4(0.25f, 0.25f, 32.25f, 32.25f),
	            GSVector4i(0, 0, 1024, 1024), GSVector4(0.00f, 0.00f, 32.00f, 32.00f),
	                                          GSVector4(0.25f, 0.25f, 31.75f, 31.75f));
	// Weird upscale
	// Input xy -0.5 to 64.5, while st are 0 to 32
	tester.test(GSVector4i( 8,  8, 1032, 1032), GSVector4(0.00f, 0.00f, 32.00f, 32.00f),
	            GSVector4i(16, 16, 1040, 1040), GSVector4(0.00f, 0.00f, 32.00f, 32.00f),
	                                            GSVector4(0.25f, 0.25f, 31.75f, 31.75f));
}

TEST(ShrinkTest, Shrink)
{
	VTTest tester;
	// Standard downsample
	// Input xy 0 to 32, while st are 1 to 65
	tester.test(GSVector4i(0, 0, 512, 512), GSVector4(1.0f, 1.0f, 65.0f, 65.0f),
	            GSVector4i(0, 0, 512, 512), GSVector4(0.0f, 0.0f, 64.0f, 64.0f),
	                                        GSVector4(1.0f, 1.0f, 63.0f, 63.0f));
	// Weird downsample
	// Input xy 0.5 to 31.5, while st are 0 to 64
	tester.test(GSVector4i( 8,  8, 520, 520), GSVector4(0.0f, 0.0f, 64.0f, 64.0f),
	            GSVector4i(16, 16, 528, 528), GSVector4(0.0f, 0.0f, 64.0f, 64.0f),
	                                          GSVector4(1.0f, 1.0f, 63.0f, 63.0f));
}

// Things needed for the linker to be happy
Pcsx2Config::GSOptions::GSOptions() {}
Pcsx2Config::GSOptions GSConfig;
