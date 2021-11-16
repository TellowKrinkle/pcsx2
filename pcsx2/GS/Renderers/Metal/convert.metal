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

#include "GSMTLShaderCommon.h"

using namespace metal;

struct ConvertVSIn
{
	vector_float2 position  [[attribute(0)]];
	vector_float2 texcoord0 [[attribute(1)]];
};

struct OSDVSIn
{
	vector_float2 position  [[attribute(0)]];
	vector_float2 texcoord0 [[attribute(1)]];
	vector_float4 color     [[attribute(2)]];
};

vertex ConvertShaderData vs_convert(ConvertVSIn in [[stage_in]])
{
	ConvertShaderData out;
	out.p = float4(in.position, 0.5f, 1.f);
	out.t = in.texcoord0;
	return out;
}

vertex OSDShaderData vs_osd(OSDVSIn in [[stage_in]])
{
	OSDShaderData out;
	out.p = float4(in.position, 0.5f, 1.f);
	out.t = in.texcoord0;
	out.c = in.color;
	return out;
}

float4 ps_crt(float4 color, int i)
{
	constexpr float4 mask[4] =
	{
		float4(1, 0, 0, 0),
		float4(0, 1, 0, 0),
		float4(0, 0, 1, 0),
		float4(1, 1, 1, 0),
	};

	return color * saturate(mask[i] + 0.5f);
}

float4 ps_scanlines(float4 color, int i)
{
	constexpr float4 mask[2] =
	{
		float4(1, 1, 1, 0),
		float4(0, 0, 0, 0)
	};

	return color * saturate(mask[i] + 0.5f);
}

fragment float4 ps_copy(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	return res.sample(data.t);
}

fragment ushort ps_convert_rgba8_16bits(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	float4 c = res.sample(data.t);
	c.a *= 256.f / 127.f; // hm, 0.5 won't give us 1.0 if we just multiply with 2

	uint4 i = uint4(c * float4(0x001f, 0x03e0, 0x7c00, 0x8000));
	return (i.x & 0x001f) | (i.y & 0x03e0) | (i.z & 0x7c00) | (i.w & 0x8000);
}

fragment void ps_datm1(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	if (res.sample(data.t).a < (127.5f / 255.f))
		discard_fragment();
}

fragment void ps_datm0(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	if (res.sample(data.t).a > (127.5f / 255.f))
		discard_fragment();
}

fragment float4 ps_mod256(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	float4 c = round(res.sample(data.t) * 255.f);
	return (c - 256.f * floor(c / 256.f)) / 255.f;
}

fragment float4 ps_filter_scanlines(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	return ps_scanlines(res.sample(data.t), uint(data.p.y) % 2);
}

fragment float4 ps_filter_diagonal(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	uint4 p = uint4(data.p);
	return ps_crt(res.sample(data.t), (p.x + (p.y % 3)) % 3);
}

fragment float4 ps_filter_transparency(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	float4 c = res.sample(data.t);
	c.a = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));
	return c;
}

fragment float4 ps_filter_triangular(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	uint4 p = uint4(data.p);
	uint val = ((p.x + ((p.y >> 1) & 1) * 3) >> 1) % 3;
	return ps_crt(res.sample(data.t), val);
}

fragment float4 ps_filter_complex(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	float2 texdim = float2(res.texture.get_width(), res.texture.get_height());

	if (dfdy(data.t.y) * texdim.y > 0.5)
	{
		return res.sample(data.t);
	}
	else
	{
		float factor = (0.9f - 0.4f * cos(2.f * M_PI_F * data.t.y * texdim.y));
		float ycoord = (floor(data.t.y * texdim.y) + 0.5f) / texdim.y;
		return factor * res.sample(float2(data.t.x, ycoord));
	}
}

fragment uint ps_convert_float32_32bits(ConvertShaderData data [[stage_in]], ConvertPSDepthRes res)
{
	return uint(0x1p32 * res.sample(data.t).r);
}

fragment float4 ps_convert_float32_rgba8(ConvertShaderData data [[stage_in]], ConvertPSDepthRes res)
{
	return convert_depth32_rgba8(res.sample(data.t).r) * (256.f/255.f);
}

fragment float4 ps_convert_float16_rgb5a1(ConvertShaderData data [[stage_in]], ConvertPSDepthRes res)
{
	return convert_depth16_rgba8(res.sample(data.t).r) / float4(32, 32, 32, 1);
}

struct DepthOut
{
	float depth [[depth(any)]];
	DepthOut(float depth): depth(depth) {}
};

fragment DepthOut ps_convert_rgba8_float32(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	constexpr float4 bitSh = float4(0xFFp-32, 0xFFp-24, 0xFFp-16, 0xFFp-8);

	return dot(res.sample(data.t), bitSh);
}

fragment DepthOut ps_convert_rgba8_float24(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	// Same as above but without the alpha channel (24 bits Z)
	constexpr float3 bitSh = float3(0xFFp-32, 0xFFp-24, 0xFFp-16);
	return dot(res.sample(data.t).rgb, bitSh);
}

fragment DepthOut ps_convert_rgba8_float16(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	// Same as above but without the A/B channels (16 bits Z)
	constexpr float2 bitSh = float2(0xFFp-32, 0xFFp-24);
	return dot(res.sample(data.t).rg, bitSh);
}

fragment DepthOut ps_convert_rgb5a1_float16(ConvertShaderData data [[stage_in]], ConvertPSRes res)
{
	constexpr float4 bitSh = float4(0x1p-32, 0x1p-27, 0x1p-22, 0x1p-17);
	// Trunc color to drop useless lsb
	float4 color = trunc(res.sample(data.t) * (float4(255) / float4(8, 8, 8, 128)));
	return dot(color, bitSh);
}

fragment float4 ps_convert_rgba_8i(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLConvertPSUniform& uniform [[buffer(GSMTLBufferIndexUniforms)]])
{
	// Convert a RGBA texture into a 8 bits packed texture
	// Input column: 8x2 RGBA pixels
	// 0: 8 RGBA
	// 1: 8 RGBA
	// Output column: 16x4 Index pixels
	// 0: 8 R | 8 B
	// 1: 8 R | 8 B
	// 2: 8 G | 8 A
	// 3: 8 G | 8 A
	float c;

	uint2 sel = uint2(data.p.xy) % uint2(16, 16);
	uint2 tb  = (uint2(data.p.xy) & ~uint2(15, 3)) >> 1;

	uint ty  = tb.y | (uint(data.p.y) & 1);
	uint txN = tb.x | (uint(data.p.x) & 7);
	uint txH = tb.x | ((uint(data.p.x) + 4) & 7);

	txN *= SCALING_FACTOR.x;
	txH *= SCALING_FACTOR.x;
	ty  *= SCALING_FACTOR.y;

	// TODO investigate texture gather
	float4 cN = res.texture.read(uint2(txN, ty));
	float4 cH = res.texture.read(uint2(txH, ty));

	if ((sel.y & 4) == 0)
	{
		// Column 0 and 2
		if ((sel.y & 2) == 0)
		{
			if ((sel.x & 8) == 0)
				c = cN.r;
			else
				c = cN.b;
		}
		else
		{
			if ((sel.x & 8) == 0)
				c = cH.g;
			else
				c = cH.a;
		}
	}
	else
	{
		// Column 1 and 3
		if ((sel.y & 2) == 0)
		{
			if ((sel.x & 8) == 0)
				c = cH.r;
			else
				c = cH.b;
		}
		else
		{
			if ((sel.x & 8) == 0)
				c = cN.g;
			else
				c = cN.a;
		}
	}
	return float4(c);
}

fragment float4 ps_yuv(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLConvertPSUniform& uniform [[buffer(GSMTLBufferIndexUniforms)]])
{
	float4 i = res.sample(data.t);
	float4 o;

	// Value from GS manual
	const float3x3 rgb2yuv =
	{
		{0.587, -0.311, -0.419},
		{0.114,  0.500, -0.081},
		{0.299, -0.169,  0.500}
	};

	float3 yuv = rgb2yuv * i.gbr;

	float Y  = 0xDB / 255.f * yuv.x + 0x10 / 255.f;
	float Cr = 0xE0 / 255.f * yuv.y + 0x80 / 255.f;
	float Cb = 0xE0 / 255.f * yuv.z + 0x80 / 255.f;

	switch (uniform.emoda)
	{
		case 0: o.a = i.a; break;
		case 1: o.a = Y;   break;
		case 2: o.a = Y/2; break;
		case 3: o.a = 0;   break;
	}

	switch (uniform.emodc)
	{
		case 0: o.rgb = i.rgb;             break;
		case 1: o.rgb = float3(Y);         break;
		case 2: o.rgb = float3(Y, Cb, Cr); break;
		case 3: o.rgb = float3(i.a);       break;
	}

	return o;
}

kernel void clear_destination_alpha(
	uint2 pos [[thread_position_in_grid]],
	constant uint& width [[buffer(GSMTLBufferIndexUniforms)]],
	device int* buffer   [[buffer(GSMTLBufferIndexPrimIDBuffer)]])
{
	buffer[pos.y * width + pos.x] = INT_MAX;
}

fragment float4 ps_osd(OSDShaderData data [[stage_in]], ConvertPSRes res)
{
	return data.c * float4(1, 1, 1, res.sample(data.t).a);
}

