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
#include <simd/simd.h>

enum GSMTLBufferIndices
{
	GSMTLBufferIndexVertices,
	GSMTLBufferIndexUniforms,
	GSMTLBufferIndexPrimIDBuffer,
};

enum GSMTLTextureIndex
{
	GSMTLTextureIndexTex,
	GSMTLTextureIndexPalette,
	GSMTLTextureIndexRenderTarget,
	GSMTLTextureIndexRawTex,
};

struct GSMTLConvertPSUniform
{
	int emoda;
	int emodc;
};

struct GSMTLInterlacePSUniform
{
	vector_float2 ZrH;
	float hH;
};

struct GSMTLMainVSUniform
{
	vector_float2 vertex_scale;
	vector_float2 vertex_offset;
	vector_float2 texture_offset;
	vector_float2 texture_scale;
	uint max_depth;
};

struct GSMTLMainPSUniform
{
	union
	{
		vector_float4 fog_color_aref;
		vector_float3 fog_color;
		struct
		{
			float pad0[3];
			float aref;
		};
	};
	vector_float4 wh; ///< xy => PS2, zw => actual (upscaled)
	vector_float4 half_texel;
	vector_float4 uv_min_max;
	vector_ushort4 uv_msk_fix;
	vector_float2 tc_offset;
	vector_float2 ta;
	vector_int2 dither_matrix;

	float max_depth;
	float alpha_fix;
	vector_uchar4 fbmask;
	union
	{
		struct
		{
			unsigned char blue_mask;
			unsigned char blue_shift;
			unsigned char green_mask;
			unsigned char green_shift;
		} channel_shuffle;
		unsigned int channel_shuffle_int;
	};
};

enum GSMTLAttributes
{
	GSMTLAttributeIndexST,
	GSMTLAttributeIndexC,
	GSMTLAttributeIndexQ,
	GSMTLAttributeIndexXY,
	GSMTLAttributeIndexZ,
	GSMTLAttributeIndexUV,
	GSMTLAttributeIndexF,
};

enum GSMTLFnConstants
{
	GSMTLConstantIndex_SCALING_FACTOR,
	GSMTLConstantIndex_FST,
	GSMTLConstantIndex_IIP,
	GSMTLConstantIndex_PS_AEM_FMT,
	GSMTLConstantIndex_PS_PAL_FMT,
	GSMTLConstantIndex_PS_DFMT,
	GSMTLConstantIndex_PS_DEPTH_FMT,
	GSMTLConstantIndex_PS_AEM,
	GSMTLConstantIndex_PS_FBA,
	GSMTLConstantIndex_PS_FOG,
	GSMTLConstantIndex_PS_DATE,
	GSMTLConstantIndex_PS_ATST,
	GSMTLConstantIndex_PS_FST,
	GSMTLConstantIndex_PS_TFX,
	GSMTLConstantIndex_PS_TCC,
	GSMTLConstantIndex_PS_WMS,
	GSMTLConstantIndex_PS_WMT,
	GSMTLConstantIndex_PS_LTF,
	GSMTLConstantIndex_PS_SHUFFLE,
	GSMTLConstantIndex_PS_READ_BA,
	GSMTLConstantIndex_PS_WRITE_RG,
	GSMTLConstantIndex_PS_FBMASK,
	GSMTLConstantIndex_PS_BLEND_A,
	GSMTLConstantIndex_PS_BLEND_B,
	GSMTLConstantIndex_PS_BLEND_C,
	GSMTLConstantIndex_PS_BLEND_D,
	GSMTLConstantIndex_PS_CLR1,
	GSMTLConstantIndex_PS_HDR,
	GSMTLConstantIndex_PS_COLCLIP,
	GSMTLConstantIndex_PS_PABE,
	GSMTLConstantIndex_PS_CHANNEL,
	GSMTLConstantIndex_PS_DITHER,
	GSMTLConstantIndex_PS_ZCLAMP,
	GSMTLConstantIndex_PS_TCOFFSETHACK,
	GSMTLConstantIndex_PS_URBAN_CHAOS_HLE,
	GSMTLConstantIndex_PS_TALES_OF_ABYSS_HLE,
	GSMTLConstantIndex_PS_TEX_IS_FB,
	GSMTLConstantIndex_PS_AUTOMATIC_LOD,
	GSMTLConstantIndex_PS_MANUAL_LOD,
	GSMTLConstantIndex_PS_POINT_SAMPLER,
	GSMTLConstantIndex_PS_INVALID_TEX0,
};
