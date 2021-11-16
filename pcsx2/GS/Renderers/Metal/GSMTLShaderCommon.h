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

#pragma once
#include <metal_stdlib>
#include "GSMTLSharedHeader.h"

using namespace metal;

constant uchar2 SCALING_FACTOR [[function_constant(GSMTLConstantIndex_SCALING_FACTOR)]];

struct ConvertShaderData
{
	float4 p [[position]];
	float2 t;
};

struct OSDShaderData
{
	float4 p [[position]];
	float2 t;
	float4 c;
};

struct ConvertPSRes
{
	texture2d<float> texture [[texture(0)]];
	sampler s [[sampler(0)]];
	float4 sample(float2 coord)
	{
		return texture.sample(s, coord);
	}
};

struct ConvertPSDepthRes
{
	depth2d<float> texture [[texture(0)]];
	sampler s [[sampler(0)]];
	float4 sample(float2 coord)
	{
		return texture.sample(s, coord);
	}
};

static inline float4 convert_depth32_rgba8(float value)
{
	constexpr float4 bitSh = float4(0x1p24, 0x1p16, 0x1p8, 0x1p0);
	constexpr float4 bitMsk = float4(0, 0x1p-8, 0x1p-8, 0x1p-8);

	float4 ret = fract(float4(value) * bitSh);
	return (ret - ret.xxyz * bitMsk);
}

static inline float4 convert_depth16_rgba8(float value)
{
	constexpr float4 bitSh = float4(0x1p32, 0x1p27, 0x1p22, 0x1p17);
	constexpr uint4 bitMsk = uint4(0x1F, 0x1F, 0x1F, 0x1);

	return float4(uint4(float4(value) * bitSh) & bitMsk);
}
