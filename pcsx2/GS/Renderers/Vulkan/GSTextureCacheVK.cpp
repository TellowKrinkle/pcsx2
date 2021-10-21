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
#include "GSTextureCacheVK.h"

// GSTextureCache11

GSTextureCacheVK::GSTextureCacheVK(GSRenderer* r)
	: GSTextureCache(r)
{
}

static void ReadbackAndCopy(GSRenderer* renderer, GSTexture* tex, GSVector4i r, const GIFRegTEX0& TEX0)
{
	GSTexture::GSMap map;
	if (g_vulkan_dev->ReadbackTexture(tex, r, 0, &map))
	{
		GSOffset off = renderer->m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
		renderer->m_mem.WritePixel32(map.bits, map.pitch, off, r);
	}
}

void GSTextureCacheVK::Read(Target* t, const GSVector4i& r)
{
	if (!t->m_dirty.empty() || r.width() == 0 || r.height() == 0)
	{
		return;
	}

	const GIFRegTEX0& TEX0 = t->m_TEX0;

	VkFormat format;
	int ps_shader;
	switch (TEX0.PSM)
	{
		case PSM_PSMCT32:
		case PSM_PSMCT24:
			{
				// faster path when not scaled
				if (t->m_texture->GetScale() == GSVector2(1.0f, 1.0f))
				{
					ReadbackAndCopy(m_renderer, t->m_texture, r, TEX0);
					return;
				}

				format = VK_FORMAT_R8G8B8A8_UNORM;
				ps_shader = ShaderConvert_COPY;
			}
			break;

		case PSM_PSMCT16:
		case PSM_PSMCT16S:
			format = VK_FORMAT_R16_UINT;
			ps_shader = ShaderConvert_RGBA8_TO_16_BITS;
			break;

		case PSM_PSMZ32:
		case PSM_PSMZ24:
			format = VK_FORMAT_R32_UINT;
			ps_shader = ShaderConvert_FLOAT32_TO_32_BITS;
			break;

		case PSM_PSMZ16:
		case PSM_PSMZ16S:
			format = VK_FORMAT_R16_UINT;
			ps_shader = ShaderConvert_FLOAT32_TO_32_BITS;
			break;

		default:
			return;
	}

	// printf("GSRenderTarget::Read %d,%d - %d,%d (%08x)\n", r.left, r.top, r.right, r.bottom, TEX0.TBP0);

	int w = r.width();
	int h = r.height();

	GSVector4 src = GSVector4(r) * GSVector4(t->m_texture->GetScale()).xyxy() / GSVector4(t->m_texture->GetSize()).xyxy();

	GSTexture* rt = g_vulkan_dev->DrawForReadback(t->m_texture, src, w, h, format, ps_shader);
	if (!rt)
		return;

	GSTexture::GSMap map;
	if (g_vulkan_dev->ReadbackTexture(rt, GSVector4i(0, 0, w, h), 0, &map))
	{
		GSOffset off = m_renderer->m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
		switch (TEX0.PSM)
		{
			case PSM_PSMCT32:
			case PSM_PSMZ32:
				m_renderer->m_mem.WritePixel32(map.bits, map.pitch, off, r);
				break;
			case PSM_PSMCT24:
			case PSM_PSMZ24:
				m_renderer->m_mem.WritePixel24(map.bits, map.pitch, off, r);
				break;
			case PSM_PSMCT16:
			case PSM_PSMCT16S:
			case PSM_PSMZ16:
			case PSM_PSMZ16S:
				m_renderer->m_mem.WritePixel16(map.bits, map.pitch, off, r);
				break;

			default:
				ASSERT(0);
				break;
		}
	}

	g_vulkan_dev->Recycle(rt);
}

void GSTextureCacheVK::Read(Source* t, const GSVector4i& r)
{
	const GIFRegTEX0& TEX0 = t->m_TEX0;
	ReadbackAndCopy(m_renderer, t->m_texture, r, TEX0);
}
