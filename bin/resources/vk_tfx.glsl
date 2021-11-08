//////////////////////////////////////////////////////////////////////
// Vertex Shader
//////////////////////////////////////////////////////////////////////

#if defined(VERTEX_SHADER) || defined(GEOMETRY_SHADER)

layout(std140, set = 0, binding = 0) uniform cb0
{
	vec4 VertexScale;
	vec4 VertexOffset;
	vec4 Texture_Scale_Offset;
	vec2 PointSize;
	uint MaxDepth;
};

#endif

#ifdef VERTEX_SHADER

layout(location = 0) in vec2 a_st;
layout(location = 1) in uvec4 a_c;
layout(location = 2) in float a_q;
layout(location = 3) in uvec2 a_p;
layout(location = 4) in uint a_z;
layout(location = 5) in uvec2 a_uv;
layout(location = 6) in vec4 a_f;

layout(location = 0) out VSOutput
{
	vec4 t;
	vec4 ti;
	vec4 c;
} vsOut;

void main()
{
	// Clamp to max depth, gs doesn't wrap
	float z = min(a_z, MaxDepth);

	// pos -= 0.05 (1/320 pixel) helps avoiding rounding problems (integral part of pos is usually 5 digits, 0.05 is about as low as we can go)
	// example: ceil(afterseveralvertextransformations(y = 133)) => 134 => line 133 stays empty
	// input granularity is 1/16 pixel, anything smaller than that won't step drawing up/left by one pixel
	// example: 133.0625 (133 + 1/16) should start from line 134, ceil(133.0625 - 0.05) still above 133

	vec4 p = vec4(a_p, z, 0) - vec4(0.05f, 0.05f, 0, 0);

	gl_Position = p * VertexScale - VertexOffset;
	gl_Position.y = -gl_Position.y;

	#if VS_TME
		vec2 uv = a_uv - Texture_Scale_Offset.zw;
		vec2 st = a_st - Texture_Scale_Offset.zw;

		// Integer nomalized
		vsOut.ti.xy = uv * Texture_Scale_Offset.xy;

		#if VS_FST
			// Integer integral
			vsOut.ti.zw = uv;
		#else
			// float for post-processing in some games
			vsOut.ti.zw = st / Texture_Scale_Offset.xy;
		#endif

		// Float coords
		vsOut.t.xy = st;
		vsOut.t.w = a_q;
	#else
		vsOut.t = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		vsOut.ti = vec4(0.0f);
	#endif

	#if VS_POINT
		gl_PointSize = VS_POINT_SIZE;
	#endif

	vsOut.c = a_c;
	vsOut.t.z = a_f.r;
}

#endif

#ifdef GEOMETRY_SHADER

layout(location = 0) in VSOutput
{
	vec4 t;
	vec4 ti;
	vec4 c;
} gsIn[];

layout(location = 0) out GSOutput
{
	vec4 t;
	vec4 ti;
	vec4 c;
} gsOut;

void WriteVertex(vec4 pos, vec4 t, vec4 ti, vec4 c)
{
	gl_Position = pos;
	gsOut.t = t;
	gsOut.ti = ti;
	gsOut.c = c;
	EmitVertex();
}

//////////////////////////////////////////////////////////////////////
// Geometry Shader
//////////////////////////////////////////////////////////////////////

#if GS_PRIM == 0 && GS_POINT == 0

layout(points) in;
layout(points, max_vertices = 1) out;
void main()
{
	WriteVertex(gl_in[0].gl_Position, gsIn[0].t, gsIn[0].ti, gsIn[0].c);
	EndPrimitive();
}

#elif GS_PRIM == 0 && GS_POINT == 1

layout(points) in;
layout(triangle_strip, max_vertices = 4) out;

void main()
{
	// Transform a point to a NxN sprite

	// Get new position
	vec4 lt_p = gl_in[0].gl_Position;
	vec4 rb_p = gl_in[0].gl_Position + vec4(PointSize.x, PointSize.y, 0.0f, 0.0f);
	vec4 lb_p = rb_p;
	vec4 rt_p = rb_p;
	lb_p.x = lt_p.x;
	rt_p.y = lt_p.y;

	WriteVertex(lt_p, gsIn[0].t, gsIn[0].ti, gsIn[0].c);
	WriteVertex(lb_p, gsIn[0].t, gsIn[0].ti, gsIn[0].c);
	WriteVertex(rt_p, gsIn[0].t, gsIn[0].ti, gsIn[0].c);
	WriteVertex(rb_p, gsIn[0].t, gsIn[0].ti, gsIn[0].c);

	EndPrimitive();
}

#elif GS_PRIM == 1 && GS_LINE == 0

layout(lines) in;
layout(line_strip, max_vertices = 2) out;

void main()
{
#if GS_IIP == 0
	WriteVertex(gl_in[0].gl_Position, gsIn[0].t, gsIn[0].ti, gsIn[1].c);
	WriteVertex(gl_in[1].gl_Position, gsIn[1].t, gsIn[1].ti, gsIn[1].c);
#else
	WriteVertex(gl_in[0].gl_Position, gsIn[0].t, gsIn[0].ti, gsIn[0].c);
	WriteVertex(gl_in[1].gl_Position, gsIn[1].t, gsIn[1].ti, gsIn[1].c);
#endif
	EndPrimitive();
}

#elif GS_PRIM == 1 && GS_LINE == 1

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

void main()
{
	// Transform a line to a thick line-sprite
	vec4 left_t = gsIn[0].t;
	vec4 left_ti = gsIn[0].ti;
	vec4 left_c = gsIn[0].c;
	vec4 right_t = gsIn[1].t;
	vec4 right_ti = gsIn[1].ti;
	vec4 right_c = gsIn[1].c;
	vec4 lt_p = gl_in[0].gl_Position;
	vec4 rt_p = gl_in[1].gl_Position;

	// Potentially there is faster math
	vec2 line_vector = normalize(rt_p.xy - lt_p.xy);
	vec2 line_normal = vec2(line_vector.y, -line_vector.x);
	vec2 line_width = (line_normal * PointSize) / 2.0;

	lt_p.xy -= line_width;
	rt_p.xy -= line_width;
	vec4 lb_p = gl_in[0].gl_Position + vec4(line_width, 0.0, 0.0);
	vec4 rb_p = gl_in[1].gl_Position + vec4(line_width, 0.0, 0.0);

	#if GS_IIP == 0
	left_c = right_c;
	#endif

	WriteVertex(lt_p, left_t, left_ti, left_c);
	WriteVertex(lb_p, left_t, left_ti, left_c);
	WriteVertex(rt_p, right_t, right_ti, right_c);
	WriteVertex(rb_p, right_t, right_ti, right_c);
	EndPrimitive();
}

#elif GS_PRIM == 2

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

void main()
{
	#if GS_IIP == 0
	WriteVertex(gl_in[0].gl_Position, gsIn[0].t, gsIn[0].ti, gsIn[2].c);
	WriteVertex(gl_in[1].gl_Position, gsIn[1].t, gsIn[1].ti, gsIn[2].c);
	WriteVertex(gl_in[2].gl_Position, gsIn[2].t, gsIn[2].ti, gsIn[2].c);
	#else
	WriteVertex(gl_in[0].gl_Position, gsIn[0].t, gsIn[0].ti, gsIn[0].c);
	WriteVertex(gl_in[1].gl_Position, gsIn[1].t, gsIn[1].ti, gsIn[0].c);
	WriteVertex(gl_in[2].gl_Position, gsIn[2].t, gsIn[2].ti, gsIn[0].c);
	#endif

	EndPrimitive();
}

#elif GS_PRIM == 3

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

void main()
{
	vec4 lt_p = gl_in[0].gl_Position;
	vec4 lt_t = gsIn[0].t;
	vec4 lt_ti = gsIn[0].ti;
	vec4 lt_c = gsIn[0].c;
	vec4 rb_p = gl_in[1].gl_Position;
	vec4 rb_t = gsIn[1].t;
	vec4 rb_ti = gsIn[1].ti;
	vec4 rb_c = gsIn[1].c;

	// flat depth
	lt_p.z = rb_p.z;
	// flat fog and texture perspective
	lt_t.zw = rb_t.zw;

	// flat color
	lt_c = rb_c;

	// Swap texture and position coordinate
	vec4 lb_p = rb_p;
	vec4 lb_t = rb_t;
	vec4 lb_ti = rb_ti;
	vec4 lb_c = rb_c;
	lb_p.x = lt_p.x;
	lb_t.x = lt_t.x;
	lb_ti.x = lt_ti.x;
	lb_ti.z = lt_ti.z;

	vec4 rt_p = rb_p;
	vec4 rt_t = rb_t;
	vec4 rt_ti = rb_ti;
	vec4 rt_c = rb_c;
	rt_p.y = lt_p.y;
	rt_t.y = lt_t.y;
	rt_ti.y = lt_ti.y;
	rt_ti.w = lt_ti.w;

	WriteVertex(lt_p, lt_t, lt_ti, lt_c);
	WriteVertex(lb_p, lb_t, lb_ti, lb_c);
	WriteVertex(rt_p, rt_t, rt_ti, rt_c);
	WriteVertex(rb_p, rb_t, rb_ti, rb_c);
	EndPrimitive();
}

#endif
#endif

#ifdef FRAGMENT_SHADER

#define FMT_32 0
#define FMT_24 1
#define FMT_16 2

#ifndef VS_TME
#define VS_TME 1
#define VS_FST 1
#endif

#ifndef GS_IIP
#define GS_IIP 0
#define GS_PRIM 3
#define GS_POINT 0
#define GS_LINE 0
#endif

#ifndef PS_FST
#define PS_FST 0
#define PS_WMS 0
#define PS_WMT 0
#define PS_FMT FMT_32
#define PS_AEM 0
#define PS_TFX 0
#define PS_TCC 1
#define PS_ATST 1
#define PS_FOG 0
#define PS_CLR1 0
#define PS_FBA 0
#define PS_FBMASK 0
#define PS_LTF 1
#define PS_TCOFFSETHACK 0
#define PS_POINT_SAMPLER 0
#define PS_SHUFFLE 0
#define PS_READ_BA 0
#define PS_DFMT 0
#define PS_DEPTH_FMT 0
#define PS_PAL_FMT 0
#define PS_CHANNEL_FETCH 0
#define PS_TALES_OF_ABYSS_HLE 0
#define PS_URBAN_CHAOS_HLE 0
#define PS_INVALID_TEX0 0
#define PS_SCALE_FACTOR 1
#define PS_HDR 0
#define PS_COLCLIP 0
#define PS_BLEND_A 0
#define PS_BLEND_B 0
#define PS_BLEND_C 0
#define PS_BLEND_D 0
#define PS_PABE 0
#define PS_DITHER 0
#define PS_ZCLAMP 0
#endif

#define SW_BLEND (PS_BLEND_A || PS_BLEND_B || PS_BLEND_D)
#define PS_AEM_FMT (PS_FMT & 3)

layout(std140, set = 0, binding = 1) uniform cb1
{
	vec3 FogColor;
	float AREF;
	vec4 HalfTexel;
	vec4 WH;
	vec4 MinMax;
	vec2 MinF;
	vec2 TA;
	uvec4 MskFix;
	ivec4 ChannelShuffle;
	uvec4 FbMask;
	vec4 TC_OffsetHack;
	float Af;
	float MaxDepthPS;
	vec2 pad_cb1;
	mat4 DitherMatrix;
};

layout(location = 0) in VSOutput
{
	vec4 t;
	vec4 ti;
	vec4 c;
} vsIn;

layout(location = 0, index = 0) out vec4 o_col0;
layout(location = 0, index = 1) out vec4 o_col1;

layout(set = 1, binding = 0) uniform sampler2D Texture;
layout(set = 1, binding = 1) uniform sampler2D Palette;
layout(set = 2, binding = 0) uniform texture2D RtSampler;
layout(set = 2, binding = 1) uniform texture2D RawTexture;


vec4 sample_c(vec2 uv)
{
#if PS_POINT_SAMPLER
		// Weird issue with ATI/AMD cards,
		// it looks like they add 127/128 of a texel to sampling coordinates
		// occasionally causing point sampling to erroneously round up.
		// I'm manually adjusting coordinates to the centre of texels here,
		// though the centre is just paranoia, the top left corner works fine.
		// As of 2018 this issue is still present.
		uv = (trunc(uv * WH.zw) + vec2(0.5, 0.5)) / WH.zw;
#endif

	return texture(Texture, uv);
}

vec4 sample_p(float u)
{
	return texture(Palette, vec2(u, 0.0f));
}

vec4 clamp_wrap_uv(vec4 uv)
{
	vec4 tex_size;

	#if PS_INVALID_TEX0
		tex_size = WH.zwzw;
	#else
		tex_size = WH.xyxy;
	#endif

	#if PS_WMS == PS_WMT
	{
		#if PS_WMS == 2
		{
			uv = clamp(uv, MinMax.xyxy, MinMax.zwzw);
		}
		#elif PS_WMS == 3
		{
			#if PS_FST == 0
			// wrap negative uv coords to avoid an off by one error that shifted
			// textures. Fixes Xenosaga's hair issue.
			uv = fract(uv);
			#endif
			uv = vec4((uvec4(uv * tex_size) & MskFix.xyxy) | MskFix.zwzw) / tex_size;
		}
		#endif
	}
	#else
	{
		#if PS_WMS == 2
		{
			uv.xz = clamp(uv.xz, MinMax.xx, MinMax.zz);
		}
		#elif PS_WMS == 3
		{
			#if PS_FST == 0
			uv.xz = fract(uv.xz);
			#endif
			uv.xz = vec2((uvec2(uv.xz * tex_size.xx) & MskFix.xx) | MskFix.zz) / tex_size.xx;
		}
		#elif PS_WMT == 2
		{
			uv.yw = clamp(uv.yw, MinMax.yy, MinMax.ww);
		}
		#elif PS_WMT == 3
		{
			#if PS_FST == 0
			uv.yw = fract(uv.yw);
			#endif
			uv.yw = vec2((uvec2(uv.yw * tex_size.yy) & MskFix.yy) | MskFix.ww) / tex_size.yy;
		}
		#endif
	}
	#endif

	return uv;
}

mat4 sample_4c(vec4 uv)
{
	mat4 c;

	c[0] = sample_c(uv.xy);
	c[1] = sample_c(uv.zy);
	c[2] = sample_c(uv.xw);
	c[3] = sample_c(uv.zw);

	return c;
}

vec4 sample_4_index(vec4 uv)
{
	vec4 c;

	c.x = sample_c(uv.xy).a;
	c.y = sample_c(uv.zy).a;
	c.z = sample_c(uv.xw).a;
	c.w = sample_c(uv.zw).a;

	// Denormalize value
	uvec4 i = uvec4(c * 255.0f + 0.5f);

	#if PS_PAL_FMT == 1
		// 4HL
		c = vec4(i & 0xFu) / 255.0f;
	#elif PS_PAL_FMT == 2
		// 4HH
		c = vec4(i >> 4u) / 255.0f;
	#endif

	// Most of texture will hit this code so keep normalized float value
	// 8 bits
	return c * 255./256 + 0.5/256;
}

mat4 sample_4p(vec4 u)
{
	mat4 c;

	c[0] = sample_p(u.x);
	c[1] = sample_p(u.y);
	c[2] = sample_p(u.z);
	c[3] = sample_p(u.w);

	return c;
}

int fetch_raw_depth(ivec2 xy)
{
	vec4 col = texelFetch(RawTexture, xy, 0);
	return int(col.r * exp2(32.0f));
}

vec4 fetch_raw_color(ivec2 xy)
{
	return texelFetch(RawTexture, xy, 0);
}

vec4 fetch_c(ivec2 uv)
{
	return texelFetch(Texture, uv, 0);
}

//////////////////////////////////////////////////////////////////////
// Depth sampling
//////////////////////////////////////////////////////////////////////

ivec2 clamp_wrap_uv_depth(ivec2 uv)
{
	ivec4 mask = ivec4(MskFix << 4);
	#if (PS_WMS == PS_WMT)
	{
		#if (PS_WMS == 2)
		{
			uv = clamp(uv, mask.xy, mask.zw);
		}
		#elif (PS_WMS == 3)
		{
			uv = (uv & mask.xy) | mask.zw;
		}
		#endif
	}
	#else
	{
		#if (PS_WMS == 2)
		{
			uv.x = clamp(uv.x, mask.x, mask.z);
		}
		#elif (PS_WMS == 3)
		{
			uv.x = (uv.x & mask.x) | mask.z;
		}
		#endif
		#if (PS_WMT == 2)
		{
			uv.y = clamp(uv.y, mask.y, mask.w);
		}
		#elif (PS_WMT == 3)
		{
			uv.y = (uv.y & mask.y) | mask.w;
		}
		#endif
	}
	#endif
	return uv;
}

vec4 sample_depth(vec2 st, vec2 pos)
{
	vec2 uv_f = vec2(clamp_wrap_uv_depth(ivec2(st))) * vec2(PS_SCALE_FACTOR) * vec2(1.0f / 16.0f);
	ivec2 uv = ivec2(uv_f);

	vec4 t = vec4(0.0f);

	#if (PS_TALES_OF_ABYSS_HLE == 1)
	{
		// Warning: UV can't be used in channel effect
		int depth = fetch_raw_depth(pos);

		// Convert msb based on the palette
		t = texelFetch(Palette, ivec2((depth >> 8) & 0xFF, 0), 0) * 255.0f;
	}
	#elif (PS_URBAN_CHAOS_HLE == 1)
	{
		// Depth buffer is read as a RGB5A1 texture. The game try to extract the green channel.
		// So it will do a first channel trick to extract lsb, value is right-shifted.
		// Then a new channel trick to extract msb which will shifted to the left.
		// OpenGL uses a vec32 format for the depth so it requires a couple of conversion.
		// To be faster both steps (msb&lsb) are done in a single pass.

		// Warning: UV can't be used in channel effect
		int depth = fetch_raw_depth(pos);

		// Convert lsb based on the palette
		t = Palette.Load(ivec3(depth & 0xFF, 0, 0)) * 255.0f;

		// Msb is easier
		float green = float(((depth >> 8) & 0xFF) * 36.0f);
		green = min(green, 255.0f);
		t.g += green;
	}
	#elif (PS_DEPTH_FMT == 1)
	{
		// Based on ps_main11 of convert

		// Convert a vec32 depth texture into a RGBA color texture
		const vec4 bitSh = vec4(exp2(24.0f), exp2(16.0f), exp2(8.0f), exp2(0.0f));
		const vec4 bitMsk = vec4(0.0, 1.0f / 256.0f, 1.0f / 256.0f, 1.0f / 256.0f);

		vec4 res = fract(vec4(fetch_c(uv).r) * bitSh);

		t = (res - res.xxyz * bitMsk) * 256.0f;
	}
	#elif (PS_DEPTH_FMT == 2)
	{
		// Based on ps_main12 of convert

		// Convert a vec32 (only 16 lsb) depth into a RGB5A1 color texture
		const vec4 bitSh = vec4(exp2(32.0f), exp2(27.0f), exp2(22.0f), exp2(17.0f));
		const uvec4 bitMsk = uvec4(0x1F, 0x1F, 0x1F, 0x1);
		uvec4 color = uvec4(vec4(fetch_c(uv).r) * bitSh) & bitMsk;

		t = vec4(color) * vec4(8.0f, 8.0f, 8.0f, 128.0f);
	}
	#elif (PS_DEPTH_FMT == 3)
	{
		// Convert a RGBA/RGB5A1 color texture into a RGBA/RGB5A1 color texture
		t = fetch_c(uv) * 255.0f;
	}
	#endif

	#if (PS_AEM_FMT == FMT_24)
	{
		t.a = ((PS_AEM == 0) || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	#elif (PS_AEM_FMT == FMT_16)
	{
		t.a = t.a >= 128.0f ? 255.0f * TA.y : ((PS_AEM == 0) || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	#endif

	return t;
}

//////////////////////////////////////////////////////////////////////
// Fetch a Single Channel
//////////////////////////////////////////////////////////////////////

vec4 fetch_red(ivec2 xy)
{
	vec4 rt;

	#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
		int depth = (fetch_raw_depth(xy)) & 0xFF;
		rt = vec4(float(depth) / 255.0f);
	#else
		rt = fetch_raw_color(xy);
	#endif

	return sample_p(rt.r) * 255.0f;
}

vec4 fetch_blue(ivec2 xy)
{
	vec4 rt;

	#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
		int depth = (fetch_raw_depth(xy) >> 16) & 0xFF;
		rt = vec4(float(depth) / 255.0f);
	#else
		rt = fetch_raw_color(xy);
	#endif

	return sample_p(rt.b) * 255.0f;
}

vec4 fetch_green(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	return sample_p(rt.g) * 255.0f;
}

vec4 fetch_alpha(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	return sample_p(rt.a) * 255.0f;
}

vec4 fetch_rgb(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	vec4 c = vec4(sample_p(rt.r).r, sample_p(rt.g).g, sample_p(rt.b).b, 1.0);
	return c * 255.0f;
}

vec4 fetch_gXbY(ivec2 xy)
{
	#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
		int depth = fetch_raw_depth(xy);
		int bg = (depth >> (8 + ChannelShuffle.w)) & 0xFF;
		return vec4(bg);
	#else
		ivec4 rt = ivec4(int(fetch_raw_color(xy) * 255.0));
		int green = (rt.g >> ChannelShuffle.w) & ChannelShuffle.z;
		int blue = (rt.b << ChannelShuffle.y) & ChannelShuffle.x;
		return vec4(float(green | blue));
	#endif
}

vec4 sample_color(vec2 st)
{
	#if PS_TCOFFSETHACK
	st += TC_OffsetHack.xy;
	#endif

	vec4 t;
	mat4 c;
	vec2 dd;

	#if PS_LTF == 0 && PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_WMS < 2 && PS_WMT < 2
	{
		c[0] = sample_c(st);
	}
	#else
	{
		vec4 uv;

		#if PS_LTF
		{
			uv = st.xyxy + HalfTexel;
			dd = fract(uv.xy * WH.zw);

			#if PS_FST == 0
			{
				dd = clamp(dd, vec2(0.0f), vec2(0.9999999f));
			}
			#endif
		}
		#else
		{
			uv = st.xyxy;
		}
		#endif

		uv = clamp_wrap_uv(uv);

#if PS_PAL_FMT != 0
			c = sample_4p(sample_4_index(uv));
#else
			c = sample_4c(uv);
#endif
	}
	#endif

	for (uint i = 0; i < 4; i++)
	{
		#if (PS_AEM_FMT == FMT_24)
			c[i].a = (PS_AEM == 0 || any(bvec3(c[i].rgb))) ? TA.x : 0.0f;
		#elif (PS_AEM_FMT == FMT_16)
			c[i].a = (c[i].a >= 0.5) ? TA.y : ((PS_AEM == 0 || any(bvec3(c[i].rgb))) ? TA.x : 0.0f);
		#endif
	}

	#if PS_LTF
	{
		t = mix(mix(c[0], c[1], dd.x), mix(c[2], c[3], dd.x), dd.y);
	}
	#else
	{
		t = c[0];
	}
	#endif

	return trunc(t * 255.0f + 0.05f);
}

vec4 tfx(vec4 T, vec4 C)
{
	vec4 C_out;
	vec4 FxT = trunc(trunc(C) * T / 128.0f);

#if (PS_TFX == 0)
	C_out = FxT;
#elif (PS_TFX == 1)
	C_out = T;
#elif (PS_TFX == 2)
	C_out.rgb = FxT.rgb + C.a;
	C_out.a = T.a + C.a;
#elif (PS_TFX == 3)
	C_out.rgb = FxT.rgb + C.a;
	C_out.a = T.a;
#else
	C_out = C;
#endif

#if (PS_TCC == 0)
	C_out.a = C.a;
#endif

#if (PS_TFX == 0) || (PS_TFX == 2) || (PS_TFX == 3)
	// Clamp only when it is useful
	C_out = min(C_out, 255.0f);
#endif

	return C_out;
}

void atst(vec4 C)
{
	float a = C.a;

	#if (PS_ATST == 0)
	{
		// nothing to do
	}
	#elif (PS_ATST == 1)
	{
		if (a > AREF) discard;
	}
	#elif (PS_ATST == 2)
	{
		if (a < AREF) discard;
	}
	#elif (PS_ATST == 3)
	{
		if (abs(a - AREF) > 0.5f) discard;
	}
	#elif (PS_ATST == 4)
	{
		if (abs(a - AREF) < 0.5f) discard;
	}
	#endif
}

vec4 fog(vec4 c, float f)
{
	#if PS_FOG
		c.rgb = trunc(mix(FogColor, c.rgb, f));
	#endif

	return c;
}

vec4 ps_color()
{
#if PS_FST == 0 && PS_INVALID_TEX0 == 1
	// Re-normalize coordinate from invalid GS to corrected texture size
	vec2 st = (vsIn.t.xy * WH.xy) / (vsIn.t.w * WH.zw);
	// no st_int yet
#elif PS_FST == 0
	vec2 st = vsIn.t.xy / vsIn.t.w;
	vec2 st_int = vsIn.ti.zw / vsIn.t.w;
#else
	vec2 st = vsIn.ti.xy;
	vec2 st_int = vsIn.ti.zw;
#endif

#if PS_CHANNEL_FETCH == 1
	vec4 T = fetch_red(ivec2(gl_FragCoord.xy));
#elif PS_CHANNEL_FETCH == 2
	vec4 T = fetch_green(ivec2(gl_FragCoord.xy));
#elif PS_CHANNEL_FETCH == 3
	vec4 T = fetch_blue(ivec2(gl_FragCoord.xy));
#elif PS_CHANNEL_FETCH == 4
	vec4 T = fetch_alpha(ivec2(gl_FragCoord.xy));
#elif PS_CHANNEL_FETCH == 5
	vec4 T = fetch_rgb(ivec2(gl_FragCoord.xy));
#elif PS_CHANNEL_FETCH == 6
	vec4 T = fetch_gXbY(ivec2(gl_FragCoord.xy));
#elif PS_DEPTH_FMT > 0
	vec4 T = sample_depth(st_int, gl_FragCoord.xy);
#else
	vec4 T = sample_color(st);
#endif

	vec4 C = tfx(T, vsIn.c);

	atst(C);

	C = fog(C, vsIn.t.z);

	#if PS_CLR1 // needed for Cd * (As/Ad/F + 1) blending modes
		C.rgb = vec3(255.0f);
	#endif

	return C;
}

void ps_fbmask(inout vec4 C, vec2 pos_xy)
{
	#if PS_FBMASK
		vec4 RT = trunc(texelFetch(RtSampler, ivec2(pos_xy), 0) * 255.0f + 0.1f);
		C = vec4((uvec4(C) & ~FbMask) | (uvec4(RT) & FbMask));
	#endif
}

void ps_dither(inout vec3 C, vec2 pos_xy)
{
	#if PS_DITHER
		ivec2 fpos;

		#if PS_DITHER == 2
			fpos = ivec2(pos_xy);
		#else
			fpos = ivec2(pos_xy / (float)PS_SCALE_FACTOR);
		#endif

		C += DitherMatrix[fpos.y & 3][fpos.x & 3];
	#endif
}

void ps_blend(inout vec4 Color, float As, vec2 pos_xy)
{
	#if SW_BLEND
		vec4 RT = trunc(texelFetch(RtSampler, ivec2(pos_xy), 0) * 255.0f + 0.1f);

		float Ad = (PS_DFMT == FMT_24) ? 1.0f : RT.a / 128.0f;

		vec3 Cd = RT.rgb;
		vec3 Cs = Color.rgb;
		vec3 Cv;

		vec3 A = (PS_BLEND_A == 0) ? Cs : ((PS_BLEND_A == 1) ? Cd : vec3(0.0f));
		vec3 B = (PS_BLEND_B == 0) ? Cs : ((PS_BLEND_B == 1) ? Cd : vec3(0.0f));
		vec3 C = vec3((PS_BLEND_C == 0) ? As : ((PS_BLEND_C == 1) ? Ad : Af));
		vec3 D = (PS_BLEND_D == 0) ? Cs : ((PS_BLEND_D == 1) ? Cd : vec3(0.0f));

		Cv = (PS_BLEND_A == PS_BLEND_B) ? D : trunc(((A - B) * C) + D);

		// PABE
		#if PS_PABE
			Cv = (Color.a >= 128.0f) ? Cv : Color.rgb;
		#endif

		// Dithering
		ps_dither(Cv, pos_xy);

		// Standard Clamp
		#if PS_COLCLIP == 0 && PS_HDR == 0
			Cv = clamp(Cv, vec3(0.0f), vec3(255.0f));
		#endif

		// In 16 bits format, only 5 bits of color are used. It impacts shadows computation of Castlevania
		#if PS_DFMT == FMT_16
			Cv = vec3(ivec3(Cv) & ivec3(0xF8));
		#elif PS_COLCLIP == 1 && PS_HDR == 0
			Cv = vec3(ivec3(Cv) & ivec3(0xFF));
		#endif

		Color.rgb = Cv;
	#endif
}

void main()
{
	vec4 C = ps_color();

	#if PS_SHUFFLE
		uvec4 denorm_c = uvec4(C);
		uvec2 denorm_TA = uvec2(vec2(TA.xy) * 255.0f + 0.5f);

		// Mask will take care of the correct destination
		#if PS_READ_BA
			C.rb = C.bb;
		#else
			C.rb = C.rr;
		#endif

		#if PS_READ_BA
			if ((denorm_c.a & 0x80u) != 0u)
				C.ga = vec2(float((denorm_c.a & 0x7Fu) | (denorm_TA.y & 0x80u)));
			else
				C.ga = vec2(float((denorm_c.a & 0x7Fu) | (denorm_TA.x & 0x80u)));
		#else
			if ((denorm_c.g & 0x80u) != 0u)
				C.ga = vec2(float((denorm_c.g & 0x7Fu) | (denorm_TA.y & 0x80u)));
			else
				C.ga = vec2(float((denorm_c.g & 0x7Fu) | (denorm_TA.x & 0x80u)));
		#endif
	#endif

	// Must be done before alpha correction
	float alpha_blend = C.a / 128.0f;

	// Alpha correction
	#if PS_DFMT == FMT_16
	{
		float A_one = 128.0f; // alpha output will be 0x80
		C.a = (PS_FBA != 0) ? A_one : step(128.0f, C.a) * A_one;
	}
	#elif (PS_DFMT == FMT_32) && PS_FBA
	{
		float A_one = 128.0f;
		if (C.a < A_one) C.a += A_one;
	}
	#endif

	#if !SW_BLEND
		ps_dither(C.rgb, gl_FragCoord.xy);
	#endif

	ps_blend(C, alpha_blend, gl_FragCoord.xy);

	ps_fbmask(C, gl_FragCoord.xy);

	// When dithering the bottom 3 bits become meaningless and cause lines in the picture
	// so we need to limit the color depth on dithered items
	// SW_BLEND already deals with this so no need to do in those cases
	#if (!SW_BLEND && PS_DITHER && PS_DFMT == FMT_16 && !PS_COLCLIP)
		C.rgb = clamp(C.rgb, vec3(0.0f), vec3(255.0f));
		C.rgb = vec3(uvec3(C.rgb) & uvec3(0xF8u));
	#endif

	o_col0 = C / 255.0f;
	o_col1 = vec4(alpha_blend);

#if PS_ZCLAMP
	gl_FragDepth = min(gl_FragCoord.z, MaxDepthPS);
#endif
}

#endif