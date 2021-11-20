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
#include "GSRendererVK.h"
#include "common/Vulkan/Context.h"
#include "common/Vulkan/Util.h"
#include "common/Align.h"

GSRendererVK::GSRendererVK(std::unique_ptr<GSDevice> dev)
	: GSRendererHW(std::move(dev), new GSTextureCacheVK(this))
{
	m_sw_blending = theApp.GetConfigI("accurate_blending_unit");

	const int upscale_multiplier = theApp.GetConfigI("upscale_multiplier");
	const bool has_large_points = g_vulkan_context->GetDeviceFeatures().largePoints;
	const float* point_range = g_vulkan_context->GetDeviceLimits().pointSizeRange;
	m_use_point_size = has_large_points && upscale_multiplier >= point_range[0] && upscale_multiplier <= point_range[1];
	Console.WriteLn(m_use_point_size ? "Using point size for upscaled points" : "Using geometry shader for upscaled points");

	ResetStates();
}

const char* GSRendererVK::GetName() const
{
	return "D3D11";
}

void GSRendererVK::SetupIA(const float& sx, const float& sy)
{
	GSDeviceVK* dev = GetDeviceVK();

	//D3D11_PRIMITIVE_TOPOLOGY t{};

	const bool unscale_pt_ln = m_userHacks_enabled_unscale_ptln && GetUpscaleMultiplier() != 1;
	const bool can_use_gs = g_vulkan_context->SupportsGeometryShaders();

	switch (m_vt.m_primclass)
	{
		case GS_POINT_CLASS:
			if (unscale_pt_ln && (m_use_point_size || can_use_gs))
			{
				m_p_sel.gs.point = !m_use_point_size && can_use_gs;
				vs_cb.PointSize = GSVector2(16.0f * sx, 16.0f * sy);
			}

			m_p_sel.vs.point = 1;
			m_p_sel.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			break;

		case GS_LINE_CLASS:
			if (unscale_pt_ln && can_use_gs)
			{
				m_p_sel.gs.line = 1;
				vs_cb.PointSize = GSVector2(16.0f * sx, 16.0f * sy);
			}

			m_p_sel.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			break;

		case GS_SPRITE_CLASS:
			// Lines: GPU conversion.
			// Triangles: CPU conversion.
			if (can_use_gs && !m_vt.m_accurate_stq && m_vertex.next > 32) // <=> 16 sprites (based on Shadow Hearts)
			{
				m_p_sel.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			}
			else
			{
				m_p_sel.gs.cpu_sprite = 1;
				Lines2Sprites();

				m_p_sel.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			}

			break;

		case GS_TRIANGLE_CLASS:
			m_p_sel.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			break;

		default:
			__assume(0);
	}

	void* ptr = NULL;

	if (dev->IAMapVertexBuffer(&ptr, sizeof(GSVertex), m_vertex.next))
	{
		GSVector4i::storent(ptr, m_vertex.buff, sizeof(GSVertex) * m_vertex.next);

		if (m_userhacks_wildhack && !m_isPackedUV_HackFlag)
		{
			GSVertex* RESTRICT d = (GSVertex*)ptr;

			for (unsigned int i = 0; i < m_vertex.next; i++)
			{
				if (PRIM->TME && PRIM->FST)
					d[i].UV &= 0x3FEF3FEF;
			}
		}

		dev->IAUnmapVertexBuffer();
	}

	dev->IASetIndexBuffer(m_index.buff, m_index.tail);
}

void GSRendererVK::EmulateZbuffer()
{
	if (m_context->TEST.ZTE)
	{
		m_p_sel.dss.ztst = m_context->TEST.ZTST;
		m_p_sel.dss.zwe = !m_context->ZBUF.ZMSK;
	}
	else
	{
		m_p_sel.dss.ztst = ZTST_ALWAYS;
	}

	// On the real GS we appear to do clamping on the max z value the format allows.
	// Clamping is done after rasterization.
	const u32 max_z = 0xFFFFFFFF >> (GSLocalMemory::m_psm[m_context->ZBUF.PSM].fmt * 8);
	const bool clamp_z = (u32)(GSVector4i(m_vt.m_max.p).z) > max_z;

	vs_cb.MaxDepth = 0xFFFFFFFF;
	//ps_cb.Af_MaxDepth.y = 1.0f;
	m_p_sel.ps.zclamp = 0;

	if (clamp_z)
	{
		if (m_vt.m_primclass == GS_SPRITE_CLASS || m_vt.m_primclass == GS_POINT_CLASS)
		{
			vs_cb.MaxDepth = max_z;
		}
		else if (!m_context->ZBUF.ZMSK)
		{
			ps_cb.Af_MaxDepth.y = max_z * ldexpf(1, -32);
			m_p_sel.ps.zclamp = 1;
		}
	}

	const GSVertex* v = &m_vertex.buff[0];
	// Minor optimization of a corner case (it allow to better emulate some alpha test effects)
	if (m_p_sel.dss.ztst == ZTST_GEQUAL && m_vt.m_eq.z && v[0].XYZ.Z == max_z)
	{
#ifdef _DEBUG
		fprintf(stdout, "%d: Optimize Z test GEQUAL to ALWAYS (%s)\n", s_n, psm_str(m_context->ZBUF.PSM));
#endif
		m_p_sel.dss.ztst = ZTST_ALWAYS;
	}
}

void GSRendererVK::EmulateTextureShuffleAndFbmask()
{
	// Uncomment to disable texture shuffle emulation.
	// m_texture_shuffle = false;

	if (m_texture_shuffle)
	{
		m_p_sel.ps.shuffle = 1;
		m_p_sel.ps.dfmt = 0;

		bool write_ba;
		bool read_ba;

		ConvertSpriteTextureShuffle(write_ba, read_ba);

		m_p_sel.ps.read_ba = read_ba;

		// Please bang my head against the wall!
		// 1/ Reduce the frame mask to a 16 bit format
		const u32& m = m_context->FRAME.FBMSK;
		const u32 fbmask = ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
		// FIXME GSVector will be nice here
		const u8 rg_mask = fbmask & 0xFF;
		const u8 ba_mask = (fbmask >> 8) & 0xFF;
		m_p_sel.bs.wrgba = 0;

		// 2 Select the new mask (Please someone put SSE here)
		if (rg_mask != 0xFF)
		{
			if (write_ba)
			{
				// fprintf(stderr, "%d: Color shuffle %s => B\n", s_n, read_ba ? "B" : "R");
				m_p_sel.bs.wb = 1;
			}
			else
			{
				// fprintf(stderr, "%d: Color shuffle %s => R\n", s_n, read_ba ? "B" : "R");
				m_p_sel.bs.wr = 1;
			}
			if (rg_mask)
				m_p_sel.ps.fbmask = 1;
		}

		if (ba_mask != 0xFF)
		{
			if (write_ba)
			{
				// fprintf(stderr, "%d: Color shuffle %s => A\n", s_n, read_ba ? "A" : "G");
				m_p_sel.bs.wa = 1;
			}
			else
			{
				// fprintf(stderr, "%d: Color shuffle %s => G\n", s_n, read_ba ? "A" : "G");
				m_p_sel.bs.wg = 1;
			}
			if (ba_mask)
				m_p_sel.ps.fbmask = 1;
		}

		if (m_p_sel.ps.fbmask && m_sw_blending)
		{
			// fprintf(stderr, "%d: FBMASK Unsafe SW emulated fb_mask:%x on tex shuffle\n", s_n, fbmask);
			ps_cb.FbMask.r = rg_mask;
			ps_cb.FbMask.g = rg_mask;
			ps_cb.FbMask.b = ba_mask;
			ps_cb.FbMask.a = ba_mask;
			m_p_sel.ps.feedback_loop = true;

			// No blending so hit unsafe path.
			if (!PRIM->ABE)
			{
#ifdef PCSX2_DEVBUILD
				Vulkan::Util::InsertDebugLabel(g_vulkan_context->GetCurrentCommandBuffer(),
					StringUtil::StdStringFromFormat(
						"FBMASK Unsafe SW emulated fb_mask:%x on tex shuffle", fbmask).c_str());
#endif
				m_require_one_barrier = true;
			}
			else
			{
#ifdef PCSX2_DEVBUILD
				Vulkan::Util::InsertDebugLabel(g_vulkan_context->GetCurrentCommandBuffer(),
					StringUtil::StdStringFromFormat(
						"FBMASK SW emulated fb_mask : % x on tex shuffle", fbmask).c_str());
#endif
				m_require_full_barrier = true;
			}
		}
		else
		{
			m_p_sel.ps.fbmask = 0;
		}
	}
	else
	{
		m_p_sel.ps.dfmt = GSLocalMemory::m_psm[m_context->FRAME.PSM].fmt;

		const GSVector4i fbmask_v = GSVector4i::load((int)m_context->FRAME.FBMSK);
		const int ff_fbmask = fbmask_v.eq8(GSVector4i::xffffffff()).mask();
		const int zero_fbmask = fbmask_v.eq8(GSVector4i::zero()).mask();

		m_p_sel.bs.wrgba = ~ff_fbmask; // Enable channel if at least 1 bit is 0

		m_p_sel.ps.fbmask = m_sw_blending && (~ff_fbmask & ~zero_fbmask & 0xF);

		if (m_p_sel.ps.fbmask)
		{
			ps_cb.FbMask = fbmask_v.u8to32();
			// Only alpha is special here, I think we can take a very unsafe shortcut
			// Alpha isn't blended on the GS but directly copyied into the RT.
			//
			// Behavior is clearly undefined however there is a high probability that
			// it will work. Masked bit will be constant and normally the same everywhere
			// RT/FS output/Cached value.

			// No blending so hit unsafe path.
			if (!PRIM->ABE || !(~ff_fbmask & ~zero_fbmask & 0x7))
			{
#ifdef PCSX2_DEVBUILD
				Vulkan::Util::InsertDebugLabel(g_vulkan_context->GetCurrentCommandBuffer(),
					StringUtil::StdStringFromFormat("FBMASK Unsafe SW emulated fb_mask:%x on %d bits format", m_context->FRAME.FBMSK,
					(GSLocalMemory::m_psm[m_context->FRAME.PSM].fmt == 2) ? 16 : 32).c_str());
#endif
				m_require_one_barrier = true;
			}
			else
			{
				// The safe and accurate path (but slow)
#ifdef PCSX2_DEVBUILD
				Vulkan::Util::InsertDebugLabel(g_vulkan_context->GetCurrentCommandBuffer(),
					StringUtil::StdStringFromFormat(
						"FBMASK SW emulated fb_mask:%x on %d bits format", m_context->FRAME.FBMSK,
					(GSLocalMemory::m_psm[m_context->FRAME.PSM].fmt == 2) ? 16 : 32).c_str());
#endif
				m_require_full_barrier = true;
			}

			m_p_sel.ps.feedback_loop = true;
		}
	}
}

void GSRendererVK::EmulateChannelShuffle(GSTexture** rt, const GSTextureCache::Source* tex)
{
	GSDeviceVK* dev = GetDeviceVK();

	// Uncomment to disable HLE emulation (allow to trace the draw call)
	// m_channel_shuffle = false;

	// First let's check we really have a channel shuffle effect
	if (m_channel_shuffle)
	{
		if (m_game.title == CRC::GT4 || m_game.title == CRC::GT3 || m_game.title == CRC::GTConcept || m_game.title == CRC::TouristTrophy)
		{
			// fprintf(stderr, "%d: Gran Turismo RGB Channel\n", s_n);
			m_p_sel.ps.channel = ChannelFetch_RGB;
			m_context->TEX0.TFX = TFX_DECAL;
			*rt = tex->m_from_target;
		}
		else if (m_game.title == CRC::Tekken5)
		{
			if (m_context->FRAME.FBW == 1)
			{
				// Used in stages: Secret Garden, Acid Rain, Moonlit Wilderness
				// fprintf(stderr, "%d: Tekken5 RGB Channel\n", s_n);
				m_p_sel.ps.channel = ChannelFetch_RGB;
				m_context->FRAME.FBMSK = 0xFF000000;
				// 12 pages: 2 calls by channel, 3 channels, 1 blit
				// Minus current draw call
				m_skip = 12 * (3 + 3 + 1) - 1;
				*rt = tex->m_from_target;
			}
			else
			{
				// Could skip model drawing if wrongly detected
				m_channel_shuffle = false;
			}
		}
		else if ((tex->m_texture->GetType() == GSTexture::DepthStencil) && !(tex->m_32_bits_fmt))
		{
			// So far 2 games hit this code path. Urban Chaos and Tales of Abyss
			// UC: will copy depth to green channel
			// ToA: will copy depth to alpha channel
			if ((m_context->FRAME.FBMSK & 0xFF0000) == 0xFF0000)
			{
				// Green channel is masked
				// fprintf(stderr, "%d: Tales Of Abyss Crazyness (MSB 16b depth to Alpha)\n", s_n);
				m_p_sel.ps.tales_of_abyss_hle = 1;
			}
			else
			{
				// fprintf(stderr, "%d: Urban Chaos Crazyness (Green extraction)\n", s_n);
				m_p_sel.ps.urban_chaos_hle = 1;
			}
		}
		else if (m_index.tail <= 64 && m_context->CLAMP.WMT == 3)
		{
			// Blood will tell. I think it is channel effect too but again
			// implemented in a different way. I don't want to add more CRC stuff. So
			// let's disable channel when the signature is different.
			//
			// Note: Tales Of Abyss and Tekken5 could hit this path too. Those games are
			// handled above.
			// fprintf(stderr, "%d: Maybe not a channel!\n", s_n);
			m_channel_shuffle = false;
		}
		else if (m_context->CLAMP.WMS == 3 && ((m_context->CLAMP.MAXU & 0x8) == 8))
		{
			// Read either blue or Alpha. Let's go for Blue ;)
			// MGS3/Kill Zone
			// fprintf(stderr, "%d: Blue channel\n", s_n);
			m_p_sel.ps.channel = ChannelFetch_BLUE;
		}
		else if (m_context->CLAMP.WMS == 3 && ((m_context->CLAMP.MINU & 0x8) == 0))
		{
			// Read either Red or Green. Let's check the V coordinate. 0-1 is likely top so
			// red. 2-3 is likely bottom so green (actually depends on texture base pointer offset)
			const bool green = PRIM->FST && (m_vertex.buff[0].V & 32);
			if (green && (m_context->FRAME.FBMSK & 0x00FFFFFF) == 0x00FFFFFF)
			{
				// Typically used in Terminator 3
				const int blue_mask = m_context->FRAME.FBMSK >> 24;
				const int green_mask = ~blue_mask & 0xFF;
				int blue_shift = -1;

				// Note: potentially we could also check the value of the clut
				switch (m_context->FRAME.FBMSK >> 24)
				{
					case 0xFF:
						ASSERT(0);
						break;
					case 0xFE:
						blue_shift = 1;
						break;
					case 0xFC:
						blue_shift = 2;
						break;
					case 0xF8:
						blue_shift = 3;
						break;
					case 0xF0:
						blue_shift = 4;
						break;
					case 0xE0:
						blue_shift = 5;
						break;
					case 0xC0:
						blue_shift = 6;
						break;
					case 0x80:
						blue_shift = 7;
						break;
					default:
						ASSERT(0);
						break;
				}

				const int green_shift = 8 - blue_shift;
				ps_cb.ChannelShuffle = GSVector4i(blue_mask, blue_shift, green_mask, green_shift);

				if (blue_shift >= 0)
				{
					// fprintf(stderr, "%d: Green/Blue channel (%d, %d)\n", s_n, blue_shift, green_shift);
					m_p_sel.ps.channel = ChannelFetch_GXBY;
					m_context->FRAME.FBMSK = 0x00FFFFFF;
				}
				else
				{
					// fprintf(stderr, "%d: Green channel (wrong mask) (fbmask %x)\n", s_n, m_context->FRAME.FBMSK >> 24);
					m_p_sel.ps.channel = ChannelFetch_GREEN;
				}
			}
			else if (green)
			{
				// fprintf(stderr, "%d: Green channel\n", s_n);
				m_p_sel.ps.channel = ChannelFetch_GREEN;
			}
			else
			{
				// Pop
				// fprintf(stderr, "%d: Red channel\n", s_n);
				m_p_sel.ps.channel = ChannelFetch_RED;
			}
		}
		else
		{
			// fprintf(stderr, "%d: Channel not supported\n", s_n);
			m_channel_shuffle = false;
		}
	}

	// Effect is really a channel shuffle effect so let's cheat a little
	if (m_channel_shuffle)
	{
		dev->PSSetShaderResource(2, tex->m_from_target);
		// Replace current draw with a fullscreen sprite
		//
		// Performance GPU note: it could be wise to reduce the size to
		// the rendered size of the framebuffer

		GSVertex* s = &m_vertex.buff[0];
		s[0].XYZ.X = (u16)(m_context->XYOFFSET.OFX + 0);
		s[1].XYZ.X = (u16)(m_context->XYOFFSET.OFX + 16384);
		s[0].XYZ.Y = (u16)(m_context->XYOFFSET.OFY + 0);
		s[1].XYZ.Y = (u16)(m_context->XYOFFSET.OFY + 16384);

		m_vertex.head = m_vertex.tail = m_vertex.next = 2;
		m_index.tail = 2;
	}
	else
	{
#ifdef _DEBUG
		//dev->PSSetShaderResource(4, NULL);
#endif
	}
}

void GSRendererVK::EmulateBlending()
{
	// Partial port of OGL SW blending. Currently only works for accumulation and non recursive blend.
	const GIFRegALPHA& ALPHA = m_context->ALPHA;
	bool sw_blending = false;

	// No blending so early exit
	if (!(PRIM->ABE || m_env.PABE.PABE || (PRIM->AA1 && m_vt.m_primclass == GS_LINE_CLASS)))
		return;

	// Compute the blending equation to detect special case
	const u8 blend_index = u8(((ALPHA.A * 3 + ALPHA.B) * 3 + ALPHA.C) * 3 + ALPHA.D);
	const int blend_flag = m_dev->GetBlendFlags(blend_index);
	if (!g_vulkan_context->SupportsDualSourceBlend())
	{
		const HWBlend blend_data = m_dev->GetBlend((m_p_sel.ps.dfmt == 1 && ALPHA.C == 1) ? (blend_index + 3) : blend_index);
		const bool dst_is_dual_src = (blend_data.dst == VK_BLEND_FACTOR_SRC1_ALPHA || blend_data.dst == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA);
		if (blend_data.src == VK_BLEND_FACTOR_SRC1_ALPHA || blend_data.dst == VK_BLEND_FACTOR_SRC1_ALPHA ||
				 blend_data.src == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA || blend_data.dst == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA)
		{
			sw_blending = true;
		}
	}

	// SW Blend is (nearly) free. Let's use it.
	const bool impossible_or_free_blend = (blend_flag & (BLEND_NO_REC | BLEND_A_MAX | BLEND_ACCU)) // Blend doesn't requires the costly barrier
		|| (m_prim_overlap == PRIM_OVERLAP_NO) // Blend can be done in a single draw
		|| (m_require_full_barrier);           // Another effect (for example fbmask) already requires a full barrier

	// Do the multiplication in shader for blending accumulation: Cs*As + Cd or Cs*Af + Cd
	bool accumulation_blend = !!(blend_flag & BLEND_ACCU);

	// Blending doesn't require barrier, or sampling of the rt
	const bool blend_non_recursive = !!(blend_flag & BLEND_NO_REC);

	// Warning no break on purpose
	// Note: the [[fallthrough]] attribute tell compilers not to complain about not having breaks.
	switch (m_sw_blending)
	{
	case ACC_BLEND_ULTRA:
		sw_blending |= true;
		[[fallthrough]];
	case ACC_BLEND_FULL:
		if (!m_vt.m_alpha.valid && (ALPHA.C == 0))
			GetAlphaMinMax();
		sw_blending |= (ALPHA.A != ALPHA.B) && ((ALPHA.C == 0 && m_vt.m_alpha.max > 128) || (ALPHA.C == 2 && ALPHA.FIX > 128u));
		[[fallthrough]];
	case ACC_BLEND_HIGH:
		sw_blending |= (ALPHA.C == 1);
		[[fallthrough]];
	case ACC_BLEND_MEDIUM:
		// Initial idea was to enable accurate blending for sprite rendering to handle
		// correctly post-processing effect. Some games (ZoE) use tons of sprites as particles.
		// In order to keep it fast, let's limit it to smaller draw call.
		sw_blending |= m_vt.m_primclass == GS_SPRITE_CLASS && m_drawlist.size() < 100;
		[[fallthrough]];
	case ACC_BLEND_BASIC:
		sw_blending |= impossible_or_free_blend;
		[[fallthrough]];
	default:
		/*sw_blending |= accumulation_blend*/;
	}

	// Color clip
	if (m_env.COLCLAMP.CLAMP == 0)
	{
		// fprintf(stderr, "%d: COLCLIP Info (Blending: %d/%d/%d/%d)\n", s_n, ALPHA.A, ALPHA.B, ALPHA.C, ALPHA.D);
		if (blend_non_recursive)
		{
			// The fastest algo that requires a single pass
			// fprintf(stderr, "%d: COLCLIP Free mode ENABLED\n", s_n);
			m_p_sel.ps.colclip = 1;
			sw_blending = true;
		}
		else if (accumulation_blend)
		{
			// fprintf(stderr, "%d: COLCLIP Fast HDR mode ENABLED\n", s_n);
			sw_blending = true;
			m_p_sel.ps.hdr = 1;
		}
		else
		{
			// fprintf(stderr, "%d: COLCLIP HDR mode ENABLED\n", s_n);
			m_p_sel.ps.hdr = 1;
		}
	}

	// Per pixel alpha blending
	if (m_env.PABE.PABE)
	{
		// Breath of Fire Dragon Quarter, Strawberry Shortcake, Super Robot Wars, Cartoon Network Racing.

		if (ALPHA.A == 0 && ALPHA.B == 1 && ALPHA.C == 0 && ALPHA.D == 1)
		{
			// this works because with PABE alpha blending is on when alpha >= 0x80, but since the pixel shader
			// cannot output anything over 0x80 (== 1.0) blending with 0x80 or turning it off gives the same result

			m_p_sel.bs.abe = 0;
			m_p_sel.bs.blend_index = 0;
		}
		if (sw_blending)
		{
			// fprintf(stderr, "%d: PABE mode ENABLED\n", s_n);
			m_p_sel.ps.pabe = 1;
		}
	}

	/*fprintf(stderr, "%d: BLEND_INFO: %d/%d/%d/%d. Clamp:%d. Prim:%d number %d (sw %d)\n",
		s_n, ALPHA.A, ALPHA.B, ALPHA.C, ALPHA.D, m_env.COLCLAMP.CLAMP, m_vt.m_primclass, m_vertex.next, sw_blending);*/

	if (sw_blending)
	{
		m_p_sel.ps.blend_a = ALPHA.A;
		m_p_sel.ps.blend_b = ALPHA.B;
		m_p_sel.ps.blend_c = ALPHA.C;
		m_p_sel.ps.blend_d = ALPHA.D;
		m_p_sel.ps.feedback_loop |= (ALPHA.A == 1 || ALPHA.B == 1 || ALPHA.C == 1 || ALPHA.D == 1);

		if (accumulation_blend)
		{
			m_p_sel.bs.abe = true;
			m_p_sel.bs.blend_index = blend_index;
			m_p_sel.bs.accu_blend = 1;

			if (ALPHA.A == 2)
			{
				// The blend unit does a reverse subtraction so it means
				// the shader must output a positive value.
				// Replace 0 - Cs by Cs - 0
				m_p_sel.ps.blend_a = ALPHA.B;
				m_p_sel.ps.blend_b = 2;
			}
			// Remove the addition/substraction from the SW blending
			m_p_sel.ps.blend_d = 2;
		}
		else
		{
			// Disable HW blending
			m_p_sel.bs.abe = 0;
			m_p_sel.bs.blend_index = 0;

			m_require_full_barrier |= !blend_non_recursive;
		}

		// Require the fix alpha vlaue
		if (ALPHA.C == 2)
			ps_cb.Af_MaxDepth.x = (float)ALPHA.FIX / 128.0f;
	}
	else
	{
		m_p_sel.ps.clr1 = !!(blend_flag & BLEND_C_CLR);
		m_p_sel.bs.abe = true;
		if (m_p_sel.ps.dfmt == 1 && ALPHA.C == 1)
		{
			// 24 bits doesn't have an alpha channel so use 1.0f fix factor as equivalent
			m_p_sel.bs.blend_index = blend_index + 3; // +3 <=> +1 on C
			m_p_sel.bs.accu_blend = true;
			SetBlendConstants(128);
		}
		else
		{
			m_p_sel.bs.blend_index = blend_index;
			if (ALPHA.C == 2)
				SetBlendConstants(ALPHA.FIX);
		}
	}
}

void GSRendererVK::EmulateTextureSampler(const GSTextureCache::Source* tex)
{
	// Warning fetch the texture PSM format rather than the context format. The latter could have been corrected in the texture cache for depth.
	//const GSLocalMemory::psm_t &psm = GSLocalMemory::m_psm[m_context->TEX0.PSM];
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[tex->m_TEX0.PSM];
	const GSLocalMemory::psm_t& cpsm = psm.pal > 0 ? GSLocalMemory::m_psm[m_context->TEX0.CPSM] : psm;

	const u8 wms = m_context->CLAMP.WMS;
	const u8 wmt = m_context->CLAMP.WMT;
	const bool complex_wms_wmt = !!((wms | wmt) & 2);

	bool bilinear = m_vt.IsLinear();
	const bool shader_emulated_sampler = tex->m_palette || cpsm.fmt != 0 || complex_wms_wmt || psm.depth;

	// 1 and 0 are equivalent
	m_p_sel.ps.wms = (wms & 2) ? wms : 0;
	m_p_sel.ps.wmt = (wmt & 2) ? wmt : 0;

	const int w = tex->m_texture->GetWidth();
	const int h = tex->m_texture->GetHeight();

	const int tw = (int)(1 << m_context->TEX0.TW);
	const int th = (int)(1 << m_context->TEX0.TH);

	const GSVector4 WH(tw, th, w, h);

	// Depth + bilinear filtering isn't done yet (And I'm not sure we need it anyway but a game will prove me wrong)
	// So of course, GTA set the linear mode, but sampling is done at texel center so it is equivalent to nearest sampling
	ASSERT(!(psm.depth && m_vt.IsLinear()));

	// Performance note:
	// 1/ Don't set 0 as it is the default value
	// 2/ Only keep aem when it is useful (avoid useless shader permutation)
	if (m_p_sel.ps.shuffle)
	{
		// Force a 32 bits access (normally shuffle is done on 16 bits)
		// m_ps_sel.fmt = 0; // removed as an optimization
		m_p_sel.ps.aem = m_env.TEXA.AEM;
		ASSERT(tex->m_target);

		// Require a float conversion if the texure is a depth otherwise uses Integral scaling
		if (psm.depth)
		{
			m_p_sel.ps.depth_fmt = (tex->m_texture->GetType() != GSTexture::DepthStencil) ? 3 : 1;
		}

		// Shuffle is a 16 bits format, so aem is always required
		const GSVector4 ta(m_env.TEXA & GSVector4i::x000000ff());
		ps_cb.MinF_TA = (GSVector4(ps_cb.MskFix) + 0.5f).xyxy(ta) / WH.xyxy(GSVector4(255, 255));

		bilinear &= m_vt.IsLinear();

		const GSVector4 half_offset = RealignTargetTextureCoordinate(tex);
		vs_cb.Texture_Scale_Offset.z = half_offset.x;
		vs_cb.Texture_Scale_Offset.w = half_offset.y;
	}
	else if (tex->m_target)
	{
		// Use an old target. AEM and index aren't resolved it must be done
		// on the GPU

		// Select the 32/24/16 bits color (AEM)
		m_p_sel.ps.fmt = cpsm.fmt;
		m_p_sel.ps.aem = m_env.TEXA.AEM;

		// Don't upload AEM if format is 32 bits
		if (cpsm.fmt)
		{
			const GSVector4 ta(m_env.TEXA & GSVector4i::x000000ff());
			ps_cb.MinF_TA = (GSVector4(ps_cb.MskFix) + 0.5f).xyxy(ta) / WH.xyxy(GSVector4(255, 255));
		}

		// Select the index format
		if (tex->m_palette)
		{
			// FIXME Potentially improve fmt field in GSLocalMemory
			if (m_context->TEX0.PSM == PSM_PSMT4HL)
				m_p_sel.ps.fmt |= 1 << 2;
			else if (m_context->TEX0.PSM == PSM_PSMT4HH)
				m_p_sel.ps.fmt |= 2 << 2;
			else
				m_p_sel.ps.fmt |= 3 << 2;

			// Alpha channel of the RT is reinterpreted as an index. Star
			// Ocean 3 uses it to emulate a stencil buffer.  It is a very
			// bad idea to force bilinear filtering on it.
			bilinear &= m_vt.IsLinear();
		}

		// Depth format
		if (tex->m_texture->GetType() == GSTexture::DepthStencil)
		{
			// Require a float conversion if the texure is a depth format
			m_p_sel.ps.depth_fmt = (psm.bpp == 16) ? 2 : 1;

			// Don't force interpolation on depth format
			bilinear &= m_vt.IsLinear();
		}
		else if (psm.depth)
		{
			// Use Integral scaling
			m_p_sel.ps.depth_fmt = 3;

			// Don't force interpolation on depth format
			bilinear &= m_vt.IsLinear();
		}

		const GSVector4 half_offset = RealignTargetTextureCoordinate(tex);
		vs_cb.Texture_Scale_Offset.z = half_offset.x;
		vs_cb.Texture_Scale_Offset.w = half_offset.y;
	}
	else if (tex->m_palette)
	{
		// Use a standard 8 bits texture. AEM is already done on the CLUT
		// Therefore you only need to set the index
		// m_ps_sel.aem     = 0; // removed as an optimization

		// Note 4 bits indexes are converted to 8 bits
		m_p_sel.ps.fmt = 3 << 2;
	}
	else
	{
		// Standard texture. Both index and AEM expansion were already done by the CPU.
		// m_ps_sel.fmt = 0; // removed as an optimization
		// m_ps_sel.aem = 0; // removed as an optimization
	}

	if (m_context->TEX0.TFX == TFX_MODULATE && m_vt.m_eq.rgba == 0xFFFF && m_vt.m_min.c.eq(GSVector4i(128)))
	{
		// Micro optimization that reduces GPU load (removes 5 instructions on the FS program)
		m_p_sel.ps.tfx = TFX_DECAL;
	}
	else
	{
		m_p_sel.ps.tfx = m_context->TEX0.TFX;
	}

	m_p_sel.ps.tcc = m_context->TEX0.TCC;

	m_p_sel.ps.ltf = bilinear && shader_emulated_sampler;

	m_p_sel.ps.point_sampler = !bilinear || shader_emulated_sampler;

	const GSVector4 TextureScale = GSVector4(0.0625f) / WH.xyxy();
	vs_cb.Texture_Scale_Offset.x = TextureScale.x;
	vs_cb.Texture_Scale_Offset.y = TextureScale.y;

	if (PRIM->FST)
	{
		//Maybe better?
		//vs_cb.TextureScale = GSVector4(1.0f / 16) * GSVector4(tex->m_texture->GetScale()).xyxy() / WH.zwzw();
		m_p_sel.ps.fst = 1;
	}

	ps_cb.WH = WH;
	ps_cb.HalfTexel = GSVector4(-0.5f, 0.5f).xxyy() / WH.zwzw();
	if (complex_wms_wmt)
	{
		ps_cb.MskFix = GSVector4i(m_context->CLAMP.MINU, m_context->CLAMP.MINV, m_context->CLAMP.MAXU, m_context->CLAMP.MAXV);
		ps_cb.MinMax = GSVector4(ps_cb.MskFix) / WH.xyxy();
	}

	// TC Offset Hack
	m_p_sel.ps.tcoffsethack = m_userhacks_tcoffset;
	ps_cb.TC_OffsetHack = GSVector4(m_userhacks_tcoffset_x, m_userhacks_tcoffset_y).xyxy() / WH.xyxy();

	// Must be done after all coordinates math
	if (m_context->HasFixedTEX0() && !PRIM->FST)
	{
		m_p_sel.ps.invalid_tex0 = 1;
		// Use invalid size to denormalize ST coordinate
		ps_cb.WH.x = (float)(1 << m_context->stack.TEX0.TW);
		ps_cb.WH.y = (float)(1 << m_context->stack.TEX0.TH);

		// We can't handle m_target with invalid_tex0 atm due to upscaling
		ASSERT(!tex->m_target);
	}

	// Only enable clamping in CLAMP mode. REGION_CLAMP will be done manually in the shader
	const bool tau = (wms != CLAMP_CLAMP);
	const bool tav = (wmt != CLAMP_CLAMP);
	bool ltf = bilinear && !shader_emulated_sampler;

	GSDeviceVK::SamplerSelector ss0(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);

	if (m_p_sel.ps.tfx != 4)
	{
		if (!(m_p_sel.ps.fmt < 3 && m_p_sel.ps.wms < 3 && m_p_sel.ps.wmt < 3))
		{
			ltf = 0;
		}

		// TODO: Anisotropy
		ss0.filter = ltf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		ss0.wrap_u = tau ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		ss0.wrap_v = tav ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	}

	GetDeviceVK()->PSSetSampler(0, ss0);
}

void GSRendererVK::SetBlendConstants(u8 afix)
{
	const float col = float(afix) / 128.0f;
	static_cast<GSDeviceVK*>(m_dev.get())->SetBlendConstants(GSVector4(col));
}

void GSRendererVK::ColorBufferBarrier(GSTexture* rt)
{
	const VkImageMemoryBarrier barrier = {
		VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		nullptr,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED,
		static_cast<GSTextureVK*>(rt)->GetTexture().GetImage(),
		{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}
	};

	vkCmdPipelineBarrier(g_vulkan_context->GetCurrentCommandBuffer(),
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_DEPENDENCY_BY_REGION_BIT,
		0, nullptr, 0, nullptr, 1, &barrier);
}

void GSRendererVK::ResetStates()
{
	m_require_one_barrier = false;
	m_require_full_barrier = false;

	m_p_sel.vs.key = 0;
	m_p_sel.gs.key = 0;
	m_p_sel.ps.key = 0;

	m_p_sel.bs.key = 0;
	m_p_sel.dss.key = 0;

	m_p_sel.key = 0;
}

void GSRendererVK::DrawPrims(GSTexture* rt, GSTexture* ds, GSTextureCache::Source* tex)
{
	GSTexture* hdr_rt = NULL;

	const GSVector2i& rtsize = ds ? ds->GetSize() : rt->GetSize();
	const GSVector2& rtscale = ds ? ds->GetScale() : rt->GetScale();

	const bool DATE = m_context->TEST.DATE && m_context->FRAME.PSM != PSM_PSMCT24;
	bool DATE_one = false;

	const bool ate_first_pass = m_context->TEST.DoFirstPass();
	const bool ate_second_pass = m_context->TEST.DoSecondPass();

	ResetStates();
	vs_cb.Texture_Scale_Offset = GSVector4(0.0f);

	ASSERT(m_dev != NULL);
	GSDeviceVK* dev = GetDeviceVK();

	// HLE implementation of the channel selection effect
	//
	// Warning it must be done at the begining because it will change the vertex list
	EmulateChannelShuffle(&rt, tex);

	// Upscaling hack to avoid various line/grid issues
	MergeSprite(tex);

	// Always check if primitive overlap as it is used in plenty of effects.
	m_prim_overlap = PrimitiveOverlap();

	// Detect framebuffer read that will need special handling
	if ((m_context->FRAME.Block() == m_context->TEX0.TBP0) && PRIM->TME && m_sw_blending)
	{
		if ((m_context->FRAME.FBMSK == 0x00FFFFFF) && (m_vt.m_primclass == GS_TRIANGLE_CLASS))
		{
			// This pattern is used by several games to emulate a stencil (shadow)
			// Ratchet & Clank, Jak do alpha integer multiplication (tfx) which is mostly equivalent to +1/-1
			// Tri-Ace (Star Ocean 3/RadiataStories/VP2) uses a palette to handle the +1/-1
			Vulkan::Util::InsertDebugLabel(g_vulkan_context->GetCurrentCommandBuffer(), "Source and Target are the same! Let's sample the framebuffer");
			m_p_sel.ps.tex_is_fb = 1;
			m_p_sel.ps.feedback_loop = true;
			m_require_full_barrier = true;
		}
		else if (m_prim_overlap != PRIM_OVERLAP_NO)
		{
			// Note: It is fine if the texture fits in a single GS page. First access will cache
			// the page in the GS texture buffer.
			Vulkan::Util::InsertDebugLabel(g_vulkan_context->GetCurrentCommandBuffer(), "ERROR: Source and Target are the same!");
		}
	}

	EmulateTextureShuffleAndFbmask();

	// DATE: selection of the algorithm.
	if (DATE)
	{
		if (m_texture_shuffle)
		{
			// DATE case not supported yet so keep using the old method.
			// Leave the check in to make sure other DATE cases are triggered correctly.
			// fprintf(stderr, "%d: DATE: With texture shuffle\n", s_n);
		}
		else if (m_p_sel.bs.wa && !m_context->TEST.ATE)
		{
			// Performance note: check alpha range with GetAlphaMinMax()
			GetAlphaMinMax();
			if (m_context->TEST.DATM && m_vt.m_alpha.max < 128)
			{
				// Only first pixel (write 0) will pass (alpha is 1)
				// fprintf(stderr, "%d: DATE: Fast with alpha %d-%d\n", s_n, m_vt.m_alpha.min, m_vt.m_alpha.max);
				DATE_one = true;
			}
			else if (!m_context->TEST.DATM && m_vt.m_alpha.min >= 128)
			{
				// Only first pixel (write 1) will pass (alpha is 0)
				// fprintf(stderr, "%d: DATE: Fast with alpha %d-%d\n", s_n, m_vt.m_alpha.min, m_vt.m_alpha.max);
				DATE_one = true;
			}
			else if ((m_vt.m_primclass == GS_SPRITE_CLASS /*&& m_drawlist.size() < 50*/) || (m_index.tail < 100))
			{
				// DATE case not supported yet so keep using the old method.
				// Leave the check in to make sure other DATE cases are triggered correctly.
				// fprintf(stderr, "%d: DATE: Slow with alpha %d-%d not supported\n", s_n, m_vt.m_alpha.min, m_vt.m_alpha.max);
			}
			else if (m_accurate_date)
			{
				// fprintf(stderr, "%d: DATE: Fast AD with alpha %d-%d\n", s_n, m_vt.m_alpha.min, m_vt.m_alpha.max);
				DATE_one = true;
			}
		}
		else if (!m_p_sel.bs.wa && !m_context->TEST.ATE)
		{
			// TODO: is it legal ? Likely but it need to be tested carefully.
		}
	}

	// Blend
	if (!IsOpaque() && rt)
	{
		EmulateBlending();
	}

	GSVector4i dRect;
	if (m_p_sel.ps.hdr || DATE)
		dRect = ComputeBoundingBox(rtscale, rtsize);

	if (m_p_sel.ps.dfmt == 1)
	{
		// Disable writing of the alpha channel
		m_p_sel.bs.wa = 0;
	}

	if (DATE)
	{
		const GSVector4 src = GSVector4(dRect) / GSVector4(rtsize.x, rtsize.y).xyxy();
		const GSVector4 dst = src * 2.0f - 1.0f;

		GSVertexPT1 vertices[] =
			{
				{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
				{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
				{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
				{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
			};

		dev->SetupDATE(rt, ds, vertices, m_context->TEST.DATM, dRect);
	}

	// om

	EmulateZbuffer();
	m_p_sel.rt = (rt != nullptr);
	m_p_sel.ds = (ds != nullptr);

	// vs

	m_p_sel.vs.tme = PRIM->TME;
	m_p_sel.vs.fst = PRIM->FST;

	// FIXME D3D11 and GL support half pixel center. Code could be easier!!!
	const float sx = 2.0f * rtscale.x / (rtsize.x << 4);
	const float sy = 2.0f * rtscale.y / (rtsize.y << 4);
	const float ox = (float)(int)m_context->XYOFFSET.OFX;
	const float oy = (float)(int)m_context->XYOFFSET.OFY;
	float ox2 = -1.0f / rtsize.x;
	float oy2 = -1.0f / rtsize.y;

	//This hack subtracts around half a pixel from OFX and OFY.
	//
	//The resulting shifted output aligns better with common blending / corona / blurring effects,
	//but introduces a few bad pixels on the edges.

	if (rt && rt->LikelyOffset && m_userHacks_HPO == 1)
	{
		ox2 *= rt->OffsetHack_modx;
		oy2 *= rt->OffsetHack_mody;
	}

	vs_cb.VertexScale = GSVector4(sx, -sy, ldexpf(1, -32), 0.0f);
	vs_cb.VertexOffset = GSVector4(ox * sx + ox2 + 1, -(oy * sy + oy2 + 1), 0.0f, -1.0f);
	// END of FIXME

	// gs

	m_p_sel.gs.iip = PRIM->IIP;
	if (g_vulkan_context->SupportsGeometryShaders())
		m_p_sel.gs.prim = m_vt.m_primclass;

	// ps

	if (DATE)
	{
		m_p_sel.dss.date = 1;
		if (DATE_one)
		{
			m_p_sel.dss.date_one = 1;
		}
	}

	m_p_sel.ps.fba = m_context->FBA.FBA;
	m_p_sel.ps.dither = m_dithering > 0 && m_p_sel.ps.dfmt == 2 && m_env.DTHE.DTHE;

	if (m_p_sel.ps.dither)
	{
		m_p_sel.ps.dither = m_dithering;
		ps_cb.DitherMatrix[0] = GSVector4(m_env.DIMX.DM00, m_env.DIMX.DM10, m_env.DIMX.DM20, m_env.DIMX.DM30);
		ps_cb.DitherMatrix[1] = GSVector4(m_env.DIMX.DM01, m_env.DIMX.DM11, m_env.DIMX.DM21, m_env.DIMX.DM31);
		ps_cb.DitherMatrix[2] = GSVector4(m_env.DIMX.DM02, m_env.DIMX.DM12, m_env.DIMX.DM22, m_env.DIMX.DM32);
		ps_cb.DitherMatrix[3] = GSVector4(m_env.DIMX.DM03, m_env.DIMX.DM13, m_env.DIMX.DM23, m_env.DIMX.DM33);
	}

	if (PRIM->FGE)
	{
		m_p_sel.ps.fog = 1;

		const GSVector4 fc = GSVector4::rgba32(m_env.FOGCOL.U32[0]);
		// Blend AREF to avoid to load a random value for alpha (dirty cache)
		ps_cb.FogColor_AREF = fc.blend32<8>(ps_cb.FogColor_AREF);
	}

	// Warning must be done after EmulateZbuffer
	// Depth test is always true so it can be executed in 2 passes (no order required) unlike color.
	// The idea is to compute first the color which is independent of the alpha test. And then do a 2nd
	// pass to handle the depth based on the alpha test.
	bool ate_RGBA_then_Z = false;
	bool ate_RGB_then_ZA = false;
	u8 ps_atst = 0;
	if (ate_first_pass & ate_second_pass)
	{
		// fprintf(stdout, "%d: Complex Alpha Test\n", s_n);
		const bool commutative_depth = (m_p_sel.dss.ztst == ZTST_GEQUAL && m_vt.m_eq.z) || (m_p_sel.dss.ztst == ZTST_ALWAYS);
		const bool commutative_alpha = (m_context->ALPHA.C != 1); // when either Alpha Src or a constant

		ate_RGBA_then_Z = (m_context->TEST.AFAIL == AFAIL_FB_ONLY) & commutative_depth;
		ate_RGB_then_ZA = (m_context->TEST.AFAIL == AFAIL_RGB_ONLY) & commutative_depth & commutative_alpha;
	}

	if (ate_RGBA_then_Z)
	{
		// fprintf(stdout, "%d: Alternate ATE handling: ate_RGBA_then_Z\n", s_n);
		// Render all color but don't update depth
		// ATE is disabled here
		m_p_sel.dss.zwe = false;
	}
	else if (ate_RGB_then_ZA)
	{
		// fprintf(stdout, "%d: Alternate ATE handling: ate_RGB_then_ZA\n", s_n);
		// Render RGB color but don't update depth/alpha
		// ATE is disabled here
		m_p_sel.dss.zwe = false;
		m_p_sel.bs.wa = false;
	}
	else
	{
		EmulateAtst(ps_cb.FogColor_AREF, ps_atst, false);
		m_p_sel.ps.atst = ps_atst;
	}

	if (tex)
	{
		EmulateTextureSampler(tex);
	}
	else
	{
		m_p_sel.ps.tfx = 4;
	}

	if (m_game.title == CRC::ICO)
	{
		const GSVertex* v = &m_vertex.buff[0];
		const GSVideoMode mode = GetVideoMode();
		if (tex && m_vt.m_primclass == GS_SPRITE_CLASS && m_vertex.next == 2 && PRIM->ABE && // Blend texture
			((v[1].U == 8200 && v[1].V == 7176 && mode == GSVideoMode::NTSC) || // at display resolution 512x448
				(v[1].U == 8200 && v[1].V == 8200 && mode == GSVideoMode::PAL)) && // at display resolution 512x512
			tex->m_TEX0.PSM == PSM_PSMT8H) // i.e. read the alpha channel of a 32 bits texture
		{
			// Note potentially we can limit to TBP0:0x2800

			// Depth buffer was moved so GS will invalide it which means a
			// downscale. ICO uses the MSB depth bits as the texture alpha
			// channel.  However this depth of field effect requires
			// texel:pixel mapping accuracy.
			//
			// Use an HLE shader to sample depth directly as the alpha channel

			// OutputDebugString("ICO HLE");

			m_p_sel.ps.depth_fmt = 1;
			m_p_sel.ps.channel = ChannelFetch_BLUE;

			//dev->PSSetShaderResource(4, ds);
			Console.Error("ICO");

			if (!tex->m_palette)
			{
				const u16 pal = GSLocalMemory::m_psm[tex->m_TEX0.PSM].pal;
				m_tc->AttachPaletteToSource(tex, pal, true);
			}
		}
	}

	// rs
	const GSVector4& hacked_scissor = m_channel_shuffle ? GSVector4(0, 0, 1024, 1024) : m_context->scissor.in;
	const GSVector4i scissor = GSVector4i(GSVector4(rtscale).xyxy() * hacked_scissor).rintersect(GSVector4i(rtsize).zwxy());
	if (tex)
	{
		dev->PSSetShaderResource(0, tex->m_texture);
		dev->PSSetShaderResource(1, tex->m_palette);
	}

	// Align the render area to 128x128, hopefully avoiding render pass restarts for small render area changes (e.g. Ratchet and Clank).
	const int render_area_alignment = 128 * GetUpscaleMultiplier();
	const GSVector4i render_area(
		Common::AlignDownPow2(scissor.left, render_area_alignment),
		Common::AlignDownPow2(scissor.top, render_area_alignment),
		std::min(Common::AlignUpPow2(scissor.right, render_area_alignment), rtsize.x),
		std::min(Common::AlignUpPow2(scissor.bottom, render_area_alignment), rtsize.y));

	GSTexture* draw_rt = rt;
	if (m_p_sel.ps.hdr)
	{
		hdr_rt = dev->CreateRenderTarget(rtsize.x, rtsize.y, VK_FORMAT_R32G32B32A32_SFLOAT);
		dev->SetupHDR(hdr_rt, rt, ds, dRect, scissor, DATE, m_p_sel.ps.feedback_loop);
		m_require_one_barrier = false;
		draw_rt = hdr_rt;
	}
	else
	{
		const bool render_area_okay = dev->CheckRenderPassArea(render_area);

		// Prefer keeping feedback loop enabled, that way we're not constantly restarting render passes
		m_p_sel.ps.feedback_loop |= render_area_okay && dev->CurrentFramebufferHasFeedbackLoop();
		dev->OMSetRenderTargets(rt, ds, scissor, m_p_sel.ps.feedback_loop);

		if (!render_area_okay || !dev->InRenderPass())
		{
			const bool new_target = (!rt || rt->CheckDiscarded()) && (!ds || ds->CheckDiscarded());
			const VkRenderPass rp = dev->GetTFXRenderPass(rt != nullptr, ds != nullptr, hdr_rt != nullptr, DATE, m_p_sel.ps.feedback_loop,
				new_target ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);

			if (new_target)
				dev->BeginClearRenderPass(rp, render_area, GSVector4::zero());
			else
				dev->BeginRenderPass(rp, render_area);
		}
	}

	SetupIA(sx, sy);

	dev->SetupVS(&vs_cb);
	dev->SetupPS(&ps_cb);
	
	if (m_p_sel.ps.feedback_loop)
		dev->PSSetShaderResource(3, draw_rt);

	// draw

	if (ate_first_pass)
	{
		if (dev->BindDrawPipeline(m_p_sel))
			SendDraw(draw_rt);
	}

	if (ate_second_pass)
	{
		ASSERT(!m_env.PABE.PABE);

		if (ate_RGBA_then_Z | ate_RGB_then_ZA)
		{
			// Enable ATE as first pass to update the depth
			// of pixels that passed the alpha test
			EmulateAtst(ps_cb.FogColor_AREF, ps_atst, false);
		}
		else
		{
			// second pass will process the pixels that failed
			// the alpha test
			EmulateAtst(ps_cb.FogColor_AREF, ps_atst, true);
		}

		m_p_sel.ps.atst = ps_atst;

		bool z = m_p_sel.dss.zwe;
		bool r = m_p_sel.bs.wr;
		bool g = m_p_sel.bs.wg;
		bool b = m_p_sel.bs.wb;
		bool a = m_p_sel.bs.wa;

		switch (m_context->TEST.AFAIL)
		{
			case AFAIL_KEEP:
				z = r = g = b = a = false;
				break; // none
			case AFAIL_FB_ONLY:
				z = false;
				break; // rgba
			case AFAIL_ZB_ONLY:
				r = g = b = a = false;
				break; // z
			case AFAIL_RGB_ONLY:
				z = a = false;
				break; // rgb
			default:
				__assume(0);
		}

		// Depth test should be disabled when depth writes are masked and similarly, Alpha test must be disabled
		// when writes to all of the alpha bits in the Framebuffer are masked.
		if (ate_RGBA_then_Z)
		{
			z = !m_context->ZBUF.ZMSK;
			r = g = b = a = false;
		}
		else if (ate_RGB_then_ZA)
		{
			z = !m_context->ZBUF.ZMSK;
			a = (m_context->FRAME.FBMSK & 0xFF000000) != 0xFF000000;
			r = g = b = false;
		}

		if (z || r || g || b || a)
		{
			m_p_sel.dss.zwe = z;
			m_p_sel.bs.wr = r;
			m_p_sel.bs.wg = g;
			m_p_sel.bs.wb = b;
			m_p_sel.bs.wa = a;

			if (dev->BindDrawPipeline(m_p_sel))
				SendDraw(draw_rt);
		}
	}

	if (hdr_rt)
	{
		dev->FinishHDR(hdr_rt, rt, ds, dRect, scissor, render_area, DATE, m_p_sel.ps.feedback_loop);
		dev->Recycle(hdr_rt);
	}
}

void GSRendererVK::SendDraw(GSTexture* rt)
{
	GSDeviceVK* dev = GetDeviceVK();

	if (!m_require_full_barrier && m_require_one_barrier)
	{
		// Need only a single barrier
		ColorBufferBarrier(rt);
		dev->DrawIndexedPrimitive();
	}
	else if (!m_require_full_barrier)
	{
		// Don't need any barrier
		dev->DrawIndexedPrimitive();
	}
	else if (m_prim_overlap == PRIM_OVERLAP_NO)
	{
		// Need full barrier but a single barrier will be enough
		ColorBufferBarrier(rt);
		dev->DrawIndexedPrimitive();
	}
	else if (m_vt.m_primclass == GS_SPRITE_CLASS)
	{
		const size_t nb_vertex = (m_vt.m_primclass == GS_SPRITE_CLASS && m_p_sel.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST) ? 2 : 6;

		Vulkan::Util::DebugScope scope(g_vulkan_context->GetCurrentCommandBuffer(), "Split the draw (SPRITE)");

		for (size_t count = 0, p = 0, n = 0; n < m_drawlist.size(); p += count, ++n)
		{
			count = m_drawlist[n] * nb_vertex;
			ColorBufferBarrier(rt);
			dev->DrawIndexedPrimitive(p, count);
		}
	}
	else
	{
		// FIXME: Investigate: a dynamic check to pack as many primitives as possibles
		// I'm nearly sure GS already have this kind of code (maybe we can adapt GSDirtyRect)
		const size_t nb_vertex = GSUtil::GetClassVertexCount(m_vt.m_primclass);

		Vulkan::Util::DebugScope scope(g_vulkan_context->GetCurrentCommandBuffer(), "Split single draw in %d draw", m_index.tail / nb_vertex);

		for (size_t p = 0; p < m_index.tail; p += nb_vertex)
		{
			ColorBufferBarrier(rt);
			dev->DrawIndexedPrimitive(p, nb_vertex);
		}
	}
}

bool GSRendererVK::IsDummyTexture() const
{
	// Texture is actually the frame buffer. Stencil emulation to compute shadow (Jak series/tri-ace game)
	// Will hit the "m_ps_sel.tex_is_fb = 1" path in the draw
	return (m_context->FRAME.Block() == m_context->TEX0.TBP0) && PRIM->TME && m_sw_blending && m_vt.m_primclass == GS_TRIANGLE_CLASS && (m_context->FRAME.FBMSK == 0x00FFFFFF);
}
