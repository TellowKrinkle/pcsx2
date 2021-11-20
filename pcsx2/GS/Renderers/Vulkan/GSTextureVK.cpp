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
#include "GSDeviceVK.h"
#include "GSTextureVK.h"
#include "common/Assertions.h"
#include "common/Vulkan/Builders.h"
#include "common/Vulkan/Context.h"
#include "common/Vulkan/Util.h"
#include "GS/GSPng.h"
#include "GS/GSPerfMon.h"

GSTextureVK::GSTextureVK(int type, Vulkan::Texture texture)
	: m_texture(std::move(texture))
{
	m_type = type;
	m_format = (int)m_texture.GetFormat();
	m_size.x = m_texture.GetWidth();
	m_size.y = m_texture.GetHeight();
}

GSTextureVK::GSTextureVK(int type, VkFormat format, Vulkan::StagingTexture staging_texture)
	: m_staging_texture(std::move(staging_texture))
{
	m_type = type;
	m_format = static_cast<int>(format);
	m_size.x = m_staging_texture.GetWidth();
	m_size.y = m_staging_texture.GetHeight();
}

GSTextureVK::~GSTextureVK()
{
	if (m_type == RenderTarget || m_type == DepthStencil)
	{
		for (const auto& [other_tex, fb, feedback] : m_framebuffers)
		{
			if (other_tex)
			{
				for (auto other_it = other_tex->m_framebuffers.begin(); other_it != other_tex->m_framebuffers.end(); ++other_it)
				{
					if (std::get<0>(*other_it) == this)
					{
						other_tex->m_framebuffers.erase(other_it);
						break;
					}
				}
			}

			g_vulkan_context->DeferFramebufferDestruction(fb);
		}
	}

	if (m_framebuffer != VK_NULL_HANDLE)
		g_vulkan_context->DeferFramebufferDestruction(m_framebuffer);
}

std::unique_ptr<GSTextureVK> GSTextureVK::Create(int type, u32 width, u32 height, u32 levels, VkFormat format)
{
	Vulkan::StagingTexture staging_texture;

	switch (type)
	{
		case Texture:
		{
			// for r8 textures, swizzle it across all 4 components. the shaders depend on it being in alpha.. why?
			static constexpr VkComponentMapping r8_swizzle = {
				VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R
			};

			Vulkan::Texture texture;
			if (!texture.Create(width, height, levels, 1, format, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				  (format == VK_FORMAT_R8_UNORM) ? &r8_swizzle : nullptr))
			{
				return {};
			}

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), texture.GetImage(), "%ux%u texture", width, height);
			return std::make_unique<GSTextureVK>(type, std::move(texture));
		}

		case Offscreen:
		{
			Vulkan::StagingTexture texture;
			if (!texture.Create(Vulkan::StagingBuffer::Type::Readback, format, width, height))
			{
				return {};
			}

			return std::make_unique<GSTextureVK>(type, format, std::move(texture));
		}

		case RenderTarget:
		{
			pxAssert(levels == 1);

			Vulkan::Texture texture;
			if (!texture.Create(width, height, levels, 1, format, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
			{
				return {};
			}

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), texture.GetImage(), "%ux%u render target", width, height);
			return std::make_unique<GSTextureVK>(type, std::move(texture));
		}

		case DepthStencil:
		{
			pxAssert(levels == 1);

			Vulkan::Texture texture;
			if (!texture.Create(width, height, levels, 1, format, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
			{
				return {};
			}

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), texture.GetImage(), "%ux%u depth stencil", width, height);
			return std::make_unique<GSTextureVK>(type, std::move(texture));
		}

		default:
			return {};
	}
}

void* GSTextureVK::GetNativeHandle() const
{
	return const_cast<Vulkan::Texture*>(&m_texture);
}

VkCommandBuffer GSTextureVK::GetCommandBufferForUpdate()
{
	const u32 frame = g_vulkan_dev->GetFrameNumber();
	if (m_type != GSTexture::Texture || frame == last_frame_used)
	{
		// Console.WriteLn("Texture update within frame, can't use do beforehand");
		g_vulkan_dev->EndRenderPass();
		return g_vulkan_context->GetCurrentCommandBuffer();
	}

	return g_vulkan_context->GetCurrentInitCommandBuffer();
}

bool GSTextureVK::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (m_type != Texture || static_cast<u32>(layer) >= m_texture.GetLevels())
		return false;

	g_perfmon.Put(GSPerfMon::TextureUploads, 1);

	const u32 width = r.width();
	const u32 height = r.height();
	const u32 row_length = static_cast<u32>(pitch) / Vulkan::Util::GetTexelSize(m_texture.GetFormat());
	const u32 required_size = static_cast<u32>(pitch) * height;
	Vulkan::StreamBuffer& buffer = g_vulkan_dev->GetTextureUploadBuffer();
	if (!buffer.ReserveMemory(required_size, g_vulkan_context->GetBufferImageGranularity()))
	{
		Console.Warning("Executing command buffer while waiting for %u bytes in texture upload buffer", required_size);
		g_vulkan_context->ExecuteCommandBuffer(false);
		if (!buffer.ReserveMemory(required_size, g_vulkan_context->GetBufferImageGranularity()))
			pxFailRel("Failed to reserve texture upload memory");
	}

	const u32 buffer_offset = buffer.GetCurrentOffset();
	std::memcpy(buffer.GetCurrentHostPointer(), data, required_size);
	buffer.CommitMemory(required_size);

	const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();
	const Vulkan::Util::DebugScope debugScope(cmdbuf, "GSTextureVK::Update({%d,%d} %dx%d Lvl:%u", r.x, r.y, r.width(), r.height(), layer);

	// first time the texture is used? don't leave it undefined
	if (m_texture.GetLayout() == VK_IMAGE_LAYOUT_UNDEFINED)
		m_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	m_texture.UpdateFromBuffer(cmdbuf, layer, 0, r.x, r.y, width, height, row_length, buffer.GetBuffer(), buffer_offset);
	m_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return true;
}

bool GSTextureVK::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (m_type == Texture && static_cast<u32>(layer) < m_texture.GetLevels())
	{
		// map for writing
		m_map_area = r ? *r : GSVector4i(0, 0, m_texture.GetWidth(), m_texture.GetHeight());
		m_map_level = layer;

		m.pitch = m_map_area.width() * Vulkan::Util::GetTexelSize(m_texture.GetFormat());

		const u32 required_size = m.pitch * m_map_area.height();
		Vulkan::StreamBuffer& buffer = g_vulkan_dev->GetTextureUploadBuffer();
		if (!buffer.ReserveMemory(required_size, g_vulkan_context->GetBufferImageGranularity()))
		{
			g_vulkan_dev->ExecuteCommandBuffer(false, "While waiting for %u bytes in texture upload buffer", required_size);
			if (!buffer.ReserveMemory(required_size, g_vulkan_context->GetBufferImageGranularity()))
				pxFailRel("Failed to reserve texture upload memory");
		}

		m.bits = static_cast<u8*>(buffer.GetCurrentHostPointer());
		return true;
	}
	else if (m_type == Offscreen && layer == 0)
	{
		// map for readback
		m.bits = reinterpret_cast<u8*>(m_staging_texture.GetMappedPointer());
		m.pitch = m_staging_texture.GetMappedStride();
		return true;
	}
	else
	{
		// not available
		return false;
	}
}

void GSTextureVK::Unmap()
{
	if (m_type == Texture)
	{
		pxAssert(m_map_level < m_texture.GetLevels());
		g_perfmon.Put(GSPerfMon::TextureUploads, 1);

		// TODO: non-tightly-packed formats
		const u32 width = static_cast<u32>(m_map_area.width());
		const u32 height = static_cast<u32>(m_map_area.height());
		const u32 required_size = width * height * Vulkan::Util::GetTexelSize(m_texture.GetFormat());
		Vulkan::StreamBuffer& buffer = g_vulkan_dev->GetTextureUploadBuffer();
		const u32 buffer_offset = buffer.GetCurrentOffset();
		buffer.CommitMemory(required_size);

		const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();
		const Vulkan::Util::DebugScope debugScope(cmdbuf, "GSTextureVK::Update({%d,%d} %dx%d Lvl:%u",
			m_map_area.x, m_map_area.y, m_map_area.width(), m_map_area.height(), m_map_level);

		// first time the texture is used? don't leave it undefined
		if (m_texture.GetLayout() == VK_IMAGE_LAYOUT_UNDEFINED)
			m_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		m_texture.UpdateFromBuffer(cmdbuf, m_map_level, 0, m_map_area.x, m_map_area.y, width, height, width, buffer.GetBuffer(), buffer_offset);
		m_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

bool GSTextureVK::Save(const std::string& fn)
{
#if 0
	D3D11_TEXTURE2D_DESC desc;

	m_texture->GetDesc(&desc);

	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	wil::com_ptr_nothrow<ID3D11Texture2D> res;
	HRESULT hr = m_dev->CreateTexture2D(&desc, nullptr, res.put());
	if (FAILED(hr))
	{
		return false;
	}

	m_ctx->CopyResource(res.get(), m_texture.get());

	if (m_desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)
	{
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.CPUAccessFlags |= D3D11_CPU_ACCESS_WRITE;

		wil::com_ptr_nothrow<ID3D11Texture2D> dst;
		hr = m_dev->CreateTexture2D(&desc, nullptr, dst.put());
		if (FAILED(hr))
		{
			return false;
		}

		D3D11_MAPPED_SUBRESOURCE sm, dm;

		hr = m_ctx->Map(res.get(), 0, D3D11_MAP_READ, 0, &sm);
		if (FAILED(hr))
		{
			return false;
		}
		auto unmap_res = wil::scope_exit([this, res]{ // Capture by value to preserve the original pointer
			m_ctx->Unmap(res.get(), 0);
		});

		hr = m_ctx->Map(dst.get(), 0, D3D11_MAP_WRITE, 0, &dm);
		if (FAILED(hr))
		{
			return false;
		}
		auto unmap_dst = wil::scope_exit([this, dst]{ // Capture by value to preserve the original pointer
			m_ctx->Unmap(dst.get(), 0);
		});

		const uint8* s = static_cast<const uint8*>(sm.pData);
		uint8* d = static_cast<uint8*>(dm.pData);

		for (uint32 y = 0; y < desc.Height; y++, s += sm.RowPitch, d += dm.RowPitch)
		{
			for (uint32 x = 0; x < desc.Width; x++)
			{
				reinterpret_cast<uint32*>(d)[x] = static_cast<uint32>(ldexpf(reinterpret_cast<const float*>(s)[x * 2], 32));
			}
		}

		res = std::move(dst);
	}

	res->GetDesc(&desc);

#ifdef ENABLE_OGL_DEBUG
	GSPng::Format format = GSPng::RGB_A_PNG;
#else
	GSPng::Format format = GSPng::RGB_PNG;
#endif
	switch (desc.Format)
	{
		case DXGI_FORMAT_A8_UNORM:
			format = GSPng::R8I_PNG;
			break;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			break;
		default:
			fprintf(stderr, "DXGI_FORMAT %d not saved to image\n", desc.Format);
			return false;
	}

	D3D11_MAPPED_SUBRESOURCE sm;
	hr = m_ctx->Map(res.get(), 0, D3D11_MAP_READ, 0, &sm);
	if (FAILED(hr))
	{
		return false;
	}

	int compression = theApp.GetConfigI("png_compression_level");
	bool success = GSPng::Save(format, fn, static_cast<uint8*>(sm.pData), desc.Width, desc.Height, sm.RowPitch, compression);

	m_ctx->Unmap(res.get(), 0);

	return success;
#else
	Console.Error("GSTextureVK::Save not implemented");
	return false;
#endif
}

bool GSTextureVK::Equal(GSTextureVK* tex)
{
	return tex && m_texture.GetImage() == tex->m_texture.GetImage();
}

void GSTextureVK::TransitionToLayout(VkImageLayout layout)
{
	m_texture.TransitionToLayout(g_vulkan_context->GetCurrentCommandBuffer(), layout);
}

VkFramebuffer GSTextureVK::GetFramebuffer(bool feedback_loop)
{
	return GetLinkedFramebuffer(nullptr, feedback_loop);
}

VkFramebuffer GSTextureVK::GetLinkedFramebuffer(GSTextureVK* depth_texture, bool feedback_loop)
{
	for (const auto& [other_tex, fb, other_feedback_loop] : m_framebuffers)
	{
		if (other_tex == depth_texture && other_feedback_loop == feedback_loop)
			return fb;
	}

	VkRenderPass rp = g_vulkan_context->GetRenderPass(
		(m_type != GSTexture::DepthStencil) ? m_texture.GetFormat() : VK_FORMAT_UNDEFINED,
		(m_type != GSTexture::DepthStencil) ? (depth_texture ? depth_texture->GetTexture().GetFormat() : VK_FORMAT_UNDEFINED) : m_texture.GetFormat(),
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		feedback_loop);
	if (!rp)
		return VK_NULL_HANDLE;

	Vulkan::FramebufferBuilder fbb;
	fbb.AddAttachment(m_texture.GetView());
	if (depth_texture)
		fbb.AddAttachment(depth_texture->m_texture.GetView());
	fbb.SetSize(m_texture.GetWidth(), m_texture.GetHeight(), m_texture.GetLayers());
	fbb.SetRenderPass(rp);

	VkFramebuffer fb = fbb.Create(g_vulkan_context->GetDevice());
	if (!fb)
		return VK_NULL_HANDLE;

	m_framebuffers.emplace_back(depth_texture, fb, feedback_loop);
	if (depth_texture)
		depth_texture->m_framebuffers.emplace_back(this, fb, feedback_loop);
	return fb;
}
