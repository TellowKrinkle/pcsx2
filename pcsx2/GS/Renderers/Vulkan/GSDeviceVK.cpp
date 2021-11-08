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
#include "common/Vulkan/Builders.h"
#include "common/Vulkan/Context.h"
#include "common/Vulkan/ShaderCache.h"
#include "common/Vulkan/SwapChain.h"
#include "common/Vulkan/Util.h"
#include "common/ScopedGuard.h"
#include "GS.h"
#include "GSDeviceVK.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "HostDisplay.h"
#include <sstream>

GSDeviceVK* g_vulkan_dev = nullptr;

static bool IsDepthConvertShader(int i)
{
	return (i == ShaderConvert_RGBA8_TO_FLOAT32 || i == ShaderConvert_RGBA8_TO_FLOAT24 ||
			i == ShaderConvert_RGBA8_TO_FLOAT16 || i == ShaderConvert_RGB5A1_TO_FLOAT16 ||
			i == ShaderConvert_DATM_0 || i == ShaderConvert_DATM_1);
}

static bool IsIntConvertShader(int i)
{
	return (i == ShaderConvert_RGBA8_TO_16_BITS || i == ShaderConvert_FLOAT32_TO_32_BITS);
}

static bool IsDATMConvertShader(int i)
{
	return (i == ShaderConvert_DATM_0 || i == ShaderConvert_DATM_1);
}

static bool IsPresentConvertShader(int i)
{
	return (i == ShaderConvert_COPY || (i >= ShaderConvert_SCANLINE && i <= ShaderConvert_COMPLEX_FILTER));
}

GSDeviceVK::GSDeviceVK()
{
	g_vulkan_dev = this;
	m_prefer_new_textures = true;

	m_mipmap = theApp.GetConfigI("mipmap");
	m_upscale_multiplier = std::max(0, theApp.GetConfigI("upscale_multiplier"));

	const BiFiltering nearest_filter = static_cast<BiFiltering>(theApp.GetConfigI("filter"));
	const int aniso_level = theApp.GetConfigI("MaxAnisotropy");
	if ((nearest_filter != BiFiltering::Nearest && !theApp.GetConfigB("paltex") && aniso_level))
		m_aniso_filter = aniso_level;
	else
		m_aniso_filter = 0;
}

GSDeviceVK::~GSDeviceVK()
{
	pxAssert(g_vulkan_dev == this);
	g_vulkan_dev = nullptr;
}

bool GSDeviceVK::Create(HostDisplay* display)
{
	if (!GSDevice::Create(display))
		return false;

	{
		// HACK: check nVIDIA
		// Note: It can cause issues on several games such as SOTC, Fatal Frame, plus it adds border offset.
		const bool disable_safe_features = theApp.GetConfigB("UserHacks") && theApp.GetConfigB("UserHacks_Disable_Safe_Features");
		m_hack_topleft_offset = (m_upscale_multiplier != 1 && /*nvidia_vendor &&*/ !disable_safe_features) ? -0.01f : 0.0f;
	}

	{
		std::optional<std::string> shader = Host::ReadResourceFileToString("vk_tfx.glsl");
		if (!shader.has_value())
			return false;
		m_tfx_source = std::move(*shader);
	}

	if (!CreateNullTexture())
	{
		Console.Error("Failed to create dummy texture");
		return false;
	}

	if (!CreatePipelineLayouts())
	{
		Console.Error("Failed to create pipeline layouts");
		return false;
	}

	if (!CreateRenderPasses())
	{
		Console.Error("Failed to create render passes");
		return false;
	}

	if (!CreateBuffers())
		return false;

	if (!CompileConvertPipelines() || !CompileInterlacePipelines() || !CompileMergePipelines())
	{
		Console.Error("Failed to compile utility pipelines");
		return false;
	}

	if (!CreatePersistentDescriptorSets())
	{
		Console.Error("Failed to create persistent descriptor sets");
		return false;
	}

	if (!CreateReadbackTexture())
	{
		Console.Error("Failed to create readback texture");
		return false;
	}

	InitializeState();
	return true;
}

void GSDeviceVK::Destroy()
{
	if (!g_vulkan_context)
		return;

	EndRenderPass();
	ExecuteCommandBuffer(true);
	DestroyResources();
}

void GSDeviceVK::ResetAPIState()
{
	EndRenderPass();
}

void GSDeviceVK::RestoreAPIState()
{
	InvalidateCachedState();
}

void GSDeviceVK::DrawPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	vkCmdDraw(g_vulkan_context->GetCurrentCommandBuffer(), m_vertex.count, 1, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	vkCmdDrawIndexed(g_vulkan_context->GetCurrentCommandBuffer(), m_index.count, 1, m_index.start, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive(int offset, int count)
{
	ASSERT(offset + count <= (int)m_index.count);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	vkCmdDrawIndexed(g_vulkan_context->GetCurrentCommandBuffer(), count, 1, m_index.start + offset, m_vertex.start, 0);
}

void GSDeviceVK::ClearRenderTarget(GSTexture* t, const GSVector4& c)
{
	EndRenderPass();

	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	alignas(16) VkClearColorValue ccv;
	GSVector4::store<true>((void*)ccv.float32, c);

	static_cast<GSTextureVK*>(t)->TransitionToLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	vkCmdClearColorImage(g_vulkan_context->GetCurrentCommandBuffer(),
		static_cast<GSTextureVK*>(t)->GetImage(),
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);

	static_cast<GSTextureVK*>(t)->TransitionToLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void GSDeviceVK::ClearRenderTarget(GSTexture* t, u32 c)
{
	ClearRenderTarget(t, GSVector4::rgba32(c) * (1.0f / 255));
}

void GSDeviceVK::ClearDepth(GSTexture* t)
{
	EndRenderPass();

	static_cast<GSTextureVK*>(t)->TransitionToLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	const VkClearDepthStencilValue dsv{0.0f, 0u};
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 1u, 0u, 1u};

	vkCmdClearDepthStencilImage(g_vulkan_context->GetCurrentCommandBuffer(),
		static_cast<GSTextureVK*>(t)->GetImage(),
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dsv, 1, &srr);

	static_cast<GSTextureVK*>(t)->TransitionToLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

void GSDeviceVK::ClearStencil(GSTexture* t, u8 c)
{
	EndRenderPass();

	static_cast<GSTextureVK*>(t)->TransitionToLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	const VkClearDepthStencilValue dsv{0.0f, static_cast<u32>(c)};
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 1u, 0u, 1u};

	vkCmdClearDepthStencilImage(g_vulkan_context->GetCurrentCommandBuffer(),
		static_cast<GSTextureVK*>(t)->GetImage(),
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dsv, 1, &srr);

	static_cast<GSTextureVK*>(t)->TransitionToLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

// TODO: Pools

GSTexture* GSDeviceVK::CreateSurface(int type, int w, int h, int format)
{
	//D3D11_TEXTURE2D_DESC desc;

	//memset(&desc, 0, sizeof(desc));

	// Texture limit for D3D10/11 min 1, max 8192 D3D10, max 16384 D3D11.
	const u32 width = std::max<u32>(1, std::min<u32>(w, g_vulkan_context->GetMaxImageDimension2D()));
	const u32 height = std::max<u32>(1, std::min<u32>(h, g_vulkan_context->GetMaxImageDimension2D()));

	// mipmap = m_mipmap > 1 || m_filter != TriFiltering::None;
	const bool mipmap = m_mipmap > 1 && type == GSTexture::Texture;
	const u32 layers = mipmap && format == VK_FORMAT_R8G8B8A8_UNORM ? (int)log2(std::max(w, h)) : 1;

	std::unique_ptr<GSTextureVK> tex = GSTextureVK::Create(type, width, height, layers, static_cast<VkFormat>(format));
	if (!tex)
		return nullptr;

	switch (type)
	{
		case GSTexture::RenderTarget:
			ClearRenderTarget(tex.get(), 0);
			break;
		case GSTexture::DepthStencil:
			ClearDepth(tex.get());
			break;
	}

	return tex.release();
}

GSTexture* GSDeviceVK::FetchSurface(int type, int w, int h, int format)
{
	if (format == 0)
		format = (type == GSTexture::DepthStencil || type == GSTexture::SparseDepthStencil) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_R8G8B8A8_UNORM;

	return GSDevice::FetchSurface(type, w, h, format);
}

GSTexture* GSDeviceVK::CopyOffscreen(GSTexture* src, const GSVector4& sRect, int w, int h, int format, int ps_shader)
{
	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetDevice(), "CopyOffScreen %dx%d %d", w, h, ps_shader);
	g_perfmon.Put(GSPerfMon::Readbacks, 1);

	GSTexture* rt = DrawForReadback(src, sRect, w, h, format, ps_shader);
	if (!rt)
		return nullptr;

	GSTexture* dst = CreateOffscreen(w, h, format);
	if (dst)
	{
		static_cast<GSTextureVK*>(dst)->GetStagingTexture().CopyFromTexture(
			static_cast<GSTextureVK*>(rt)->GetTexture(), 0, 0, 0, 0, 0, 0, w, h);
		ExecuteCommandBuffer(true);
	}

	Recycle(rt);
	return dst;
}

GSTexture* GSDeviceVK::DrawForReadback(GSTexture* src, const GSVector4& sRect, int w, int h, int format, int ps_shader)
{
	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetDevice(), "DrawForReadback %dx%d %d", w, h, ps_shader);

	if (format == 0)
		format = VK_FORMAT_R8G8B8A8_UNORM;

	pxAssert(format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R16_UINT || format == VK_FORMAT_R32_UINT);

	GSTextureVK* rt = static_cast<GSTextureVK*>(CreateRenderTarget(w, h, format));
	if (!rt)
		return nullptr;

	// this is the only place we use int formats, and the lookup is fine because this is gonna be slow anyway
	VkRenderPass render_pass = g_vulkan_context->GetRenderPass(static_cast<VkFormat>(format), VK_FORMAT_UNDEFINED,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	pxAssert(render_pass != VK_NULL_HANDLE);

	const GSVector4 dRect(0, 0, w, h);
	const GSVector4i dRecti(0, 0, w, h);

	EndRenderPass();

	VkFramebuffer fb = rt->GetFramebuffer();
	if (fb == VK_NULL_HANDLE)
	{
		Recycle(rt);
		return nullptr;
	}

	rt->TransitionToLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	SetFramebuffer(fb);
	SetUtilityTexture(src, m_linear_sampler);
	BeginRenderPass(render_pass, dRecti);
	SetViewportAndScissor(dRecti);
	SetPipeline(m_convert[ps_shader]);
	DrawStretchRect(sRect, dRect, rt->GetSize());
	EndRenderPass();

	return rt;
}

bool GSDeviceVK::ReadbackTexture(GSTexture* src, const GSVector4i& rect, u32 level, GSTexture::GSMap* dst)
{
	const u32 width = rect.width();
	const u32 height = rect.height();
	const u32 pitch = width * Vulkan::Util::GetTexelSize(static_cast<VkFormat>(src->GetFormat()));
	const u32 size = pitch * height;
	if (size > m_readback_staging_buffer.GetSize())
	{
		Console.Error("Can't read back %ux%u", width, height);
		return false;
	}

	g_perfmon.Put(GSPerfMon::Readbacks, 1);
	EndRenderPass();
	{
		const VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
		const Vulkan::Util::DebugScope debugScope(cmdbuf,
			"ReadbackTexture: {%d,%d} %ux%u", rect.left, rect.top, width, height);

		GSTextureVK* vkSrc = static_cast<GSTextureVK*>(src);
		VkImageLayout old_layout = vkSrc->GetTexture().GetLayout();
		if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			vkSrc->GetTexture().TransitionSubresourcesToLayout(cmdbuf, level, 1, 0, 1, old_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkBufferImageCopy image_copy = {};
		const VkImageAspectFlags aspect =
			Vulkan::Util::IsDepthFormat(static_cast<VkFormat>(vkSrc->GetFormat())) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		image_copy.bufferOffset = 0;
		image_copy.bufferRowLength = width;
		image_copy.bufferImageHeight = 0;
		image_copy.imageSubresource = {aspect, level, 0u, 1u};
		image_copy.imageOffset = {rect.left, rect.top, 0};
		image_copy.imageExtent = {width, height, 1u};

		m_readback_staging_buffer.PrepareForGPUWrite(cmdbuf, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, size);

		vkCmdCopyImageToBuffer(cmdbuf, vkSrc->GetTexture().GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			m_readback_staging_buffer.GetBuffer(), 1, &image_copy);

		m_readback_staging_buffer.FlushGPUCache(cmdbuf, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, size);

		if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			vkSrc->GetTexture().TransitionSubresourcesToLayout(cmdbuf, level, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, old_layout);
	}

	ExecuteCommandBuffer(true);

	m_readback_staging_buffer.InvalidateCPUCache(0, size);
	dst->bits = reinterpret_cast<u8*>(m_readback_staging_buffer.GetMapPointer());
	dst->pitch = pitch;

	return true;
}

void GSDeviceVK::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r)
{
	if (!sTex || !dTex)
	{
		ASSERT(0);
		return;
	}

	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	const VkImageAspectFlags src_aspect = (sTex->GetType() == GSTexture::DepthStencil) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageAspectFlags dst_aspect = (dTex->GetType() == GSTexture::DepthStencil) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageCopy ic = {
		{src_aspect, 0u, 0u, 1u},
		{r.left, r.top, 0u},
		{dst_aspect, 0u, 0u, 1u},
		{0u, 0u, 0u},
		{static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1u}};

	EndRenderPass();

	GSTextureVK* sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* dTexVK = static_cast<GSTextureVK*>(dTex);
	sTexVK->TransitionToLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	dTexVK->TransitionToLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyImage(g_vulkan_context->GetCurrentCommandBuffer(),
		sTexVK->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dTexVK->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &ic);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, int shader, bool linear)
{
	pxAssert(IsDepthConvertShader(shader) == (dTex && dTex->GetType() == GSTexture::DepthStencil));
	pxAssert(!IsIntConvertShader(shader));

	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetCurrentCommandBuffer(),
		"StretchRect(%d) {%d,%d} %dx%d -> {%d,%d) %dx%d", shader,
		int(sRect.left), int(sRect.top), int(sRect.right - sRect.left), int(sRect.bottom - sRect.top),
		int(dRect.left), int(dRect.top), int(dRect.right - dRect.left), int(dRect.bottom - dRect.top));

	DoStretchRect(sTex, sRect, dTex, dRect, dTex ? m_convert[shader] : m_present[shader], linear);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha)
{
	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetCurrentCommandBuffer(),
		"ColorCopy Red:%d Green:%d Blue:%d Alpha:%d", red, green, blue, alpha);

	const u32 index = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	DoStretchRect(sTex, sRect, dTex, dRect, m_color_copy[index], false);
}

void GSDeviceVK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, VkPipeline pipeline, bool linear)
{
	// blitting to current rt?
	const VkFramebuffer fb = dTex ? static_cast<GSTextureVK*>(dTex)->GetFramebuffer() : VK_NULL_HANDLE;
	const bool blitting_to_current_rt = !dTex || (InRenderPass() && m_current_framebuffer == fb);
	if (!blitting_to_current_rt)
	{
		if (fb == VK_NULL_HANDLE)
			return;

		EndRenderPass();
		static_cast<GSTextureVK*>(dTex)->TransitionToLayout(
			(dTex->GetType() == GSTexture::DepthStencil) ?
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}

	// can't be inside the render pass
	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	SetPipeline(pipeline);

	// TODO: Size optimization here too...
	const GSVector2i size(dTex ? dTex->GetSize() : GSVector2i(m_display->GetWindowWidth(), m_display->GetWindowHeight()));
	if (!blitting_to_current_rt)
	{
		const GSVector4i dst_rc(dRect);
		const GSVector4i tex_rc(0, 0, size.x, size.y);
		const bool is_whole_target = dTex->CheckDiscarded() || dst_rc.eq(tex_rc);

		SetFramebuffer(fb);
		if (dTex->GetType() == GSTexture::DepthStencil)
		{
			BeginRenderPass(is_whole_target ? m_utility_depth_render_pass_discard : m_utility_depth_render_pass_load, tex_rc);
		}
		else if (dTex->GetFormat() == RT_FORMAT)
		{
			BeginRenderPass(is_whole_target ? m_utility_color_render_pass_discard : m_utility_color_render_pass_load, tex_rc);
		}
		else
		{
			// blit to hdr target
			const VkRenderPass rp = g_vulkan_context->GetRenderPass(static_cast<VkFormat>(dTex->GetFormat()), VK_FORMAT_UNDEFINED,
				is_whole_target ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD);
			BeginRenderPass(rp, tex_rc);
		}

		SetViewportAndScissor(tex_rc);
	}

	DrawStretchRect(sRect, dRect, size);

	if (!blitting_to_current_rt)
	{
		EndRenderPass();
		static_cast<GSTextureVK*>(dTex)->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

void GSDeviceVK::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	// ia
	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] =
		{
			{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
			{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
			{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
			{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
		};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));

	if (ApplyUtilityState())
		DrawPrimitive();
}

void GSDeviceVK::BlitRect(GSTexture* sTex, const GSVector4i& sRect, u32 sLevel, GSTexture* dTex, const GSVector4i& dRect, u32 dLevel, bool linear)
{
	GSTextureVK* sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* dTexVK = static_cast<GSTextureVK*>(dTex);

	//const VkImageLayout old_src_layout = sTexVK->GetTexture().GetLayout();
	//const VkImageLayout old_dst_layout = dTexVK->GetTexture().GetLayout();

	EndRenderPass();

	sTexVK->TransitionToLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	dTexVK->TransitionToLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	pxAssert((sTexVK->GetType() == GSTexture::DepthStencil) == (dTexVK->GetType() == GSTexture::DepthStencil));
	const VkImageAspectFlags aspect = (sTexVK->GetType() == GSTexture::DepthStencil) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageBlit ib{
		{aspect, sLevel, 0u, 1u},
		{{sRect.left, sRect.top, 0}, {sRect.right, sRect.bottom, 1}},
		{aspect, dLevel, 0u, 1u},
		{{dRect.left, dRect.top, 0}, {dRect.right, dRect.bottom, 1}}};

	vkCmdBlitImage(g_vulkan_context->GetCurrentCommandBuffer(),
		sTexVK->GetTexture().GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dTexVK->GetTexture().GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &ib, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void GSDeviceVK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, const GSVector4& c)
{
	const bool slbg = PMODE.SLBG;
	const bool mmod = PMODE.MMOD;

	// TODO: Size optimization
	const GSVector2i size(dTex->GetSize());

	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetCurrentCommandBuffer(),
		"DoMerge %d %d", (sTex[1] && slbg), (sTex[0] != nullptr));

	const VkFramebuffer fb = static_cast<GSTextureVK*>(dTex)->GetFramebuffer();
	if (fb == VK_NULL_HANDLE)
		return;

	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	SetFramebuffer(fb);
	SetViewportAndScissor(GSVector4i(0, 0, size.x, size.y));

	// has to happen outside of the render pass
	if (sTex[1] && !slbg)
		static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (sTex[0])
		static_cast<GSTextureVK*>(sTex[0])->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	BeginClearRenderPass(m_utility_color_render_pass_clear, GSVector4i(0, 0, size.x, size.y), c);

	if (sTex[1] && !slbg)
	{
		SetUtilityTexture(sTex[1], m_linear_sampler);
		SetPipeline(m_merge[0]);
		DrawStretchRect(sRect[1], dRect[1], dTex->GetSize());
	}

	if (sTex[0])
	{
		SetUtilityTexture(sTex[0], m_linear_sampler);
		SetPipeline(m_merge[mmod ? 1 : 0]);
		SetUtilityPushConstants(&c, sizeof(c));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void GSDeviceVK::DoInterlace(GSTexture* sTex, GSTexture* dTex, int shader, bool linear, float yoffset)
{
	const GSVector2i size(dTex->GetSize());
	const GSVector4 s = GSVector4(size);

	const GSVector4 sRect(0, 0, 1, 1);
	const GSVector4 dRect(0.0f, yoffset, s.x, s.y + yoffset);

	InterlaceConstantBuffer cb;
	cb.ZrH = GSVector2(0, 1.0f / s.y);
	cb.hH = s.y / 2;

	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetCurrentCommandBuffer(),
		"DoInterlace %dx%d Shader:%d Linear:%d", size.x, size.y, shader, linear);

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	const VkFramebuffer fb = static_cast<GSTextureVK*>(dTex)->GetFramebuffer();
	if (fb == VK_NULL_HANDLE)
		return;

	const GSVector4i rc(0, 0, size.x, size.y);
	EndRenderPass();
	SetFramebuffer(fb);
	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	SetViewportAndScissor(rc);
	BeginRenderPass(m_utility_color_render_pass_load, rc);
	SetPipeline(m_interlace[shader]);
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void GSDeviceVK::SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, bool datm, const GSVector4i& bbox)
{
	const Vulkan::Util::DebugScope debugScope(
		g_vulkan_context->GetCurrentCommandBuffer(), "SetupDATE {%d,%d} %dx%d",
		bbox.left, bbox.top, bbox.width(), bbox.height());

	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	const GSVector2i size(ds->GetSize());
	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler);
	OMSetRenderTargets(nullptr, ds);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPipeline(m_convert[datm ? ShaderConvert_DATM_1 : ShaderConvert_DATM_0]);
	BeginClearRenderPass(m_date_setup_render_pass, bbox, GSVector4::zero());
	SetViewportFromRect(GSVector4i(0, 0, size.x, size.y));
	SetScissor(bbox);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

void GSDeviceVK::SetupHDR(GSTexture* hdr_rt, GSTexture* rt, GSTexture* ds, const GSVector4i& rect, const GSVector4i& scissor, bool date)
{
	const GSVector2i rtsize(rt->GetSize());
	GSTextureVK* const vkHdrRt = static_cast<GSTextureVK*>(hdr_rt);
	GSTextureVK* const vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* const vkDs = static_cast<GSTextureVK*>(ds);

	EndRenderPass();
	vkHdrRt->TransitionToLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkRt->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (vkDs)
		vkDs->TransitionToLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	// we can use an even more efficient render here by intersecting the draw area with the scissor
	const GSVector4i actual_rect(rect.rintersect(scissor));

	SetFramebuffer(ds ? vkHdrRt->GetLinkedFramebuffer(static_cast<GSTextureVK*>(ds)) : vkHdrRt->GetFramebuffer());
	SetUtilityTexture(rt, m_point_sampler);
	SetViewportFromRect(GSVector4i(0, 0, rtsize.x, rtsize.y));
	SetScissor(actual_rect);
	BeginRenderPass(GetTFXRenderPass(true, ds != nullptr, true, date, VK_ATTACHMENT_LOAD_OP_DONT_CARE), actual_rect);

	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetDevice(),
		"Create RT copy for HDR render {%d,%d} %dx%d [%dx%d]",
		actual_rect.left, actual_rect.top, actual_rect.width(), actual_rect.height(), rtsize.x, rtsize.y);

	SetPipeline(ds ? m_hdr_depth_setup_pipeline : m_hdr_no_depth_setup_pipeline);

	const GSVector4 sRect(GSVector4(actual_rect) / GSVector4(rtsize.x, rtsize.y).xyxy());
	DrawStretchRect(sRect, GSVector4(actual_rect), rtsize);
}

void GSDeviceVK::FinishHDR(GSTexture* hdr_rt, GSTexture* rt, GSTexture* ds, const GSVector4i& rect, const GSVector4i& scissor, const GSVector4i& render_area, bool date)
{
	const GSVector2i rtsize(rt->GetSize());
	GSTextureVK* const vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* const vkHdrRt = static_cast<GSTextureVK*>(hdr_rt);
	GSTextureVK* const vkDs = static_cast<GSTextureVK*>(ds);

	EndRenderPass();
	vkHdrRt->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkRt->TransitionToLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (vkDs)
		vkDs->TransitionToLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	SetFramebuffer(ds ? vkRt->GetLinkedFramebuffer(static_cast<GSTextureVK*>(ds)) : vkRt->GetFramebuffer());
	SetUtilityTexture(hdr_rt, m_point_sampler);
	SetViewportFromRect(GSVector4i(0, 0, rtsize.x, rtsize.y));
	SetScissor(scissor);
	BeginRenderPass(GetTFXRenderPass(true, ds != nullptr, false, date, VK_ATTACHMENT_LOAD_OP_LOAD), render_area);

	const GSVector4i actual_rect(rect.rintersect(scissor));
	const GSVector4 sRect(GSVector4(actual_rect) / GSVector4(rtsize.x, rtsize.y).xyxy());

	const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetDevice(),
		"Restore RT copy for HDR render {%d,%d} %dx%d [%dx%d]",
		actual_rect.left, actual_rect.top, actual_rect.width(), actual_rect.height(), rtsize.x, rtsize.y);

	SetPipeline(ds ? m_hdr_depth_finish_pipeline : m_hdr_no_depth_finish_pipeline);
	DrawStretchRect(sRect, GSVector4(actual_rect), rtsize);
}

void GSDeviceVK::IASetVertexBuffer(const void* vertex, size_t stride, size_t count)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
	{
		ExecuteCommandBuffer(false, "Uploading %u bytes to vertex buffer", size);
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.limit = count;
	m_vertex.stride = stride;
	m_vertex.count = count;
	SetVertexBuffer(m_vertex_stream_buffer.GetBuffer(), 0);

	GSVector4i::storent(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, count * stride);
	m_vertex_stream_buffer.CommitMemory(size);
}

bool GSDeviceVK::IAMapVertexBuffer(void** vertex, size_t stride, size_t count)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
	{
		ExecuteCommandBuffer(false, "Mapping %u bytes to vertex buffer", size);
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.limit = m_vertex_stream_buffer.GetCurrentSpace() / stride;
	m_vertex.stride = stride;
	m_vertex.count = count;
	SetVertexBuffer(m_vertex_stream_buffer.GetBuffer(), 0);

	*vertex = m_vertex_stream_buffer.GetCurrentHostPointer();
	return true;
}

void GSDeviceVK::IAUnmapVertexBuffer()
{
	const u32 size = static_cast<u32>(m_vertex.stride) * static_cast<u32>(m_vertex.count);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDeviceVK::IASetIndexBuffer(const void* index, size_t count)
{
	const u32 size = sizeof(u32) * static_cast<u32>(count);
	if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u32)))
	{
		ExecuteCommandBuffer(false, "Uploading %u bytes to index buffer", size);
		if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u32)))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u32);
	m_index.limit = count;
	m_index.count = count;
	SetIndexBuffer(m_index_stream_buffer.GetBuffer(), 0, VK_INDEX_TYPE_UINT32);

	std::memcpy(m_index_stream_buffer.GetCurrentHostPointer(), index, size);
	m_index_stream_buffer.CommitMemory(size);
}


void GSDeviceVK::PSSetShaderResources(GSTexture* sr0, GSTexture* sr1)
{
	pxFailRel("Should not be used");
}

void GSDeviceVK::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);
	VkFramebuffer fb = VK_NULL_HANDLE;

	pxAssert(rt || ds);
	if (vkRt && vkDs)
		fb = vkRt->GetLinkedFramebuffer(vkDs);
	else if (vkRt)
		fb = vkRt->GetFramebuffer();
	else
		fb = vkDs->GetFramebuffer();

	if (fb != m_current_framebuffer)
	{
		// if we're not the current framebuffer, make sure we're ready to go
		EndRenderPass();
		SetFramebuffer(fb);
	}

	if (!InRenderPass())
	{
		if (vkRt)
			vkRt->TransitionToLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		if (vkDs)
			vkDs->TransitionToLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	// This is used to set/initialize the framebuffer for tfx rendering.
	const GSVector2i size = rt ? rt->GetSize() : ds->GetSize();
	const VkViewport vp{m_hack_topleft_offset, m_hack_topleft_offset,
		static_cast<float>(size.x), static_cast<float>(size.y),
		0.0f, 1.0f};

	SetViewport(vp);

	if (scissor)
		SetScissor(*scissor);
	else
		SetScissor(GSVector4i(0, 0, size.x, size.y));
}

void GSDeviceVK::SetupVS(const VSConstantBuffer* cb)
{
	if (!m_vs_cb_cache.Update(cb))
		return;

	if (!m_vertex_uniform_stream_buffer.ReserveMemory(sizeof(VSConstantBuffer), g_vulkan_context->GetUniformBufferAlignment()))
	{
		ExecuteCommandBuffer(false, "Waiting for space in vertex uniform stream buffer");
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(sizeof(VSConstantBuffer), g_vulkan_context->GetUniformBufferAlignment()))
			pxFailRel("Failed to reserve vertex uniform space");
	}

	std::memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), cb, sizeof(VSConstantBuffer));
	m_tfx_dynamic_offsets[0] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
	m_vertex_uniform_stream_buffer.CommitMemory(sizeof(VSConstantBuffer));
}

void GSDeviceVK::SetupPS(const PSConstantBuffer* cb)
{
	if (!m_ps_cb_cache.Update(cb))
		return;

	if (!m_fragment_uniform_stream_buffer.ReserveMemory(sizeof(PSConstantBuffer), g_vulkan_context->GetUniformBufferAlignment()))
	{
		ExecuteCommandBuffer(false, "Waiting for space in fragment uniform stream buffer");
		if (!m_fragment_uniform_stream_buffer.ReserveMemory(sizeof(PSConstantBuffer), g_vulkan_context->GetUniformBufferAlignment()))
			pxFailRel("Failed to reserve vertex uniform space");
	}

	std::memcpy(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), cb, sizeof(PSConstantBuffer));
	m_tfx_dynamic_offsets[1] = m_fragment_uniform_stream_buffer.GetCurrentOffset();
	m_fragment_uniform_stream_buffer.CommitMemory(sizeof(PSConstantBuffer));
}

u16 GSDeviceVK::ConvertBlendEnum(u16 generic)
{
	switch (generic)
	{
		case SRC_COLOR:
			return VK_BLEND_FACTOR_SRC_COLOR;
		case INV_SRC_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case DST_COLOR:
			return VK_BLEND_FACTOR_DST_COLOR;
		case INV_DST_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case SRC1_COLOR:
			return VK_BLEND_FACTOR_SRC1_COLOR;
		case INV_SRC1_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		case SRC_ALPHA:
			return VK_BLEND_FACTOR_SRC_ALPHA;
		case INV_SRC_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case DST_ALPHA:
			return VK_BLEND_FACTOR_DST_ALPHA;
		case INV_DST_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case SRC1_ALPHA:
			return VK_BLEND_FACTOR_SRC1_ALPHA;
		case INV_SRC1_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
		case CONST_COLOR:
			return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case INV_CONST_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case CONST_ONE:
			return VK_BLEND_FACTOR_ONE;
		case CONST_ZERO:
			return VK_BLEND_FACTOR_ZERO;
		case OP_ADD:
			return VK_BLEND_OP_ADD;
		case OP_SUBTRACT:
			return VK_BLEND_OP_SUBTRACT;
		case OP_REV_SUBTRACT:
			return VK_BLEND_OP_REVERSE_SUBTRACT;
		default:
			ASSERT(0);
			return 0;
	}
}

VkSampler GSDeviceVK::GetSampler(SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
		return it->second;

	const bool aniso = (ss.filter != VK_FILTER_NEAREST && ss.anisotropy > 1);

	const VkSamplerCreateInfo ci = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		nullptr,
		0,
		static_cast<VkFilter>(ss.filter), // min
		static_cast<VkFilter>(ss.filter), // max
		VK_SAMPLER_MIPMAP_MODE_NEAREST, // mip
		static_cast<VkSamplerAddressMode>(ss.wrap_u), // u
		static_cast<VkSamplerAddressMode>(ss.wrap_v), // v
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // w
		0.0f, // lod bias
		static_cast<VkBool32>(aniso), // anisotropy enable
		static_cast<float>(ss.anisotropy), // anisotropy
		VK_FALSE, // compare enable
		VK_COMPARE_OP_ALWAYS, // compare op
		FLT_MIN, // min lod
		FLT_MAX, // max lod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // border
		VK_FALSE // unnormalized coordinates
	};
	VkSampler sampler = VK_NULL_HANDLE;
	VkResult res = vkCreateSampler(g_vulkan_context->GetDevice(), &ci, nullptr, &sampler);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkCreateSampler() failed: ");

	m_samplers.emplace(ss.key, sampler);
	return sampler;
}

static void AddMacro(std::stringstream& ss, const char* name, const char* value)
{
	ss << "#define " << name << " " << value << "\n";
}

static void AddMacro(std::stringstream& ss, const char* name, int value)
{
	ss << "#define " << name << " " << value << "\n";
}

static void AddShaderHeader(std::stringstream& ss)
{
	ss << "#version 460 core\n";
	ss << "#extension GL_EXT_samplerless_texture_functions : require\n";
}

static void AddShaderStageMacro(std::stringstream& ss, bool vs, bool gs, bool fs)
{
	if (vs)
		ss << "#define VERTEX_SHADER 1\n";
	else if (gs)
		ss << "#define GEOMETRY_SHADER 1\n";
	else if (fs)
		ss << "#define FRAGMENT_SHADER 1\n";
}

static void AddUtilityVertexAttributes(Vulkan::GraphicsPipelineBuilder& gpb)
{
	gpb.AddVertexBuffer(0, sizeof(GSVertexPT1));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
	gpb.AddVertexAttribute(2, 0, VK_FORMAT_R8G8B8A8_UNORM, 28);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
}

VkShaderModule GSDeviceVK::GetUtilityVertexShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	AddMacro(ss, "PS_SCALE_FACTOR", std::max(1, m_upscale_multiplier));
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetVertexShader(ss.str());
}

VkShaderModule GSDeviceVK::GetUtilityFragmentShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	AddMacro(ss, "PS_SCALE_FACTOR", std::max(1, m_upscale_multiplier));
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetFragmentShader(ss.str());
}

bool GSDeviceVK::CreateNullTexture()
{
	if (!m_null_texture.Create(1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
	{
		return false;
	}

	const VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	const VkClearColorValue ccv{};
	m_null_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdClearColorImage(cmdbuf, m_null_texture.GetImage(), m_null_texture.GetLayout(), &ccv, 1, &srr);
	m_null_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_null_texture.GetImage(), "Null texture");
	Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_null_texture.GetView(), "Null texture view");
	return true;
}

bool GSDeviceVK::CreateBuffers()
{
	if (!m_texture_upload_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate vertex buffer");
		return false;
	}

	if (!m_vertex_stream_buffer.Create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VERTEX_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate vertex buffer");
		return false;
	}

	if (!m_index_stream_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate index buffer");
		return false;
	}

	if (!m_vertex_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate vertex uniform buffer");
		return false;
	}

	if (!m_fragment_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate fragment uniform buffer");
		return false;
	}

	return true;
}

bool GSDeviceVK::CreatePipelineLayouts()
{
	VkDevice dev = g_vulkan_context->GetDevice();
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	//////////////////////////////////////////////////////////////////////////
	// Convert Pipeline Layout
	//////////////////////////////////////////////////////////////////////////

	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NUM_CONVERT_SAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_utility_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::Util::SetObjectName(dev, m_utility_ds_layout, "Convert descriptor layout");

	plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CONVERT_PUSH_CONSTANTS_SIZE);
	plb.AddDescriptorSet(m_utility_ds_layout);
	if ((m_utility_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::Util::SetObjectName(dev, m_utility_ds_layout, "Convert pipeline layout");

	//////////////////////////////////////////////////////////////////////////
	// Draw/TFX Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_ubo_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::Util::SetObjectName(dev, m_tfx_ubo_ds_layout, "TFX UBO descriptor layout");
	for (u32 i = 0; i < NUM_TFX_SAMPLERS; i++)
		dslb.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_sampler_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::Util::SetObjectName(dev, m_tfx_sampler_ds_layout, "TFX sampler descriptor layout");
	for (u32 i = 0; i < NUM_TFX_RT_TEXTURES; i++)
		dslb.AddBinding(i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_rt_texture_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::Util::SetObjectName(dev, m_tfx_rt_texture_ds_layout, "TFX RT texture descriptor layout");

	plb.AddDescriptorSet(m_tfx_ubo_ds_layout);
	plb.AddDescriptorSet(m_tfx_sampler_ds_layout);
	plb.AddDescriptorSet(m_tfx_rt_texture_ds_layout);
	if ((m_tfx_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::Util::SetObjectName(dev, m_tfx_pipeline_layout, "TFX pipeline layout");
	return true;
}

bool GSDeviceVK::CreateRenderPasses()
{
#define GET(dest, rt, depth, date, opa) \
	do \
	{ \
		dest = g_vulkan_context->GetRenderPass( \
			(rt), (depth), \
			((rt) != VK_FORMAT_UNDEFINED) ? (opa) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* color load */ \
			((rt) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, /* color store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opa) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* depth load */ \
			((depth) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, /* depth store */ \
			(date) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* stencil load */ \
			(date) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE /* stencil store */ \
		); \
		if (dest == VK_NULL_HANDLE) \
			return false; \
	} while (0)

	for (u32 rt = 0; rt < 2; rt++)
	{
		for (u32 ds = 0; ds < 2; ds++)
		{
			for (u32 hdr = 0; hdr < 2; hdr++)
			{
				for (u32 date = 0; date < 2; date++)
				{
					for (u32 op = VK_ATTACHMENT_LOAD_OP_LOAD; op <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; op++)
					{
						const VkFormat rt_format = (rt != 0) ? ((hdr != 0) ? HDR_RT_FORMAT : RT_FORMAT) : VK_FORMAT_UNDEFINED;
						const VkFormat depth_format = (ds != 0) ? DEPTH_FORMAT : VK_FORMAT_UNDEFINED;
						GET(m_tfx_render_pass[rt][ds][hdr][date][op],
							rt_format, depth_format, (date != 0),
							static_cast<VkAttachmentLoadOp>(op));
					}
				}
			}
		}
	}

	GET(m_utility_color_render_pass_load, RT_FORMAT, VK_FORMAT_UNDEFINED, false, VK_ATTACHMENT_LOAD_OP_LOAD);
	GET(m_utility_color_render_pass_clear, RT_FORMAT, VK_FORMAT_UNDEFINED, false, VK_ATTACHMENT_LOAD_OP_CLEAR);
	GET(m_utility_color_render_pass_discard, RT_FORMAT, VK_FORMAT_UNDEFINED, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_load, VK_FORMAT_UNDEFINED, DEPTH_FORMAT, false, VK_ATTACHMENT_LOAD_OP_LOAD);
	GET(m_utility_depth_render_pass_discard, VK_FORMAT_UNDEFINED, DEPTH_FORMAT, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

	m_date_setup_render_pass = g_vulkan_context->GetRenderPass(VK_FORMAT_UNDEFINED, DEPTH_FORMAT,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
	if (m_date_setup_render_pass == VK_NULL_HANDLE)
		return false;

#undef GET

	return true;
}

bool GSDeviceVK::CompileConvertPipelines()
{
	// we may not have a swap chain if running in headless mode.
	Vulkan::SwapChain* swapchain = static_cast<Vulkan::SwapChain*>(m_display->GetRenderSurface());
	if (swapchain)
	{
		m_swap_chain_render_pass = g_vulkan_context->GetRenderPass(swapchain->GetSurfaceFormat().format, VK_FORMAT_UNDEFINED);
		if (!m_swap_chain_render_pass)
			return false;
	}

	std::optional<std::string> shader = Host::ReadResourceFileToString("vk_convert.glsl");
	if (!shader)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([&vs]() { Vulkan::Util::SafeDestroyShaderModule(vs); });

	Vulkan::GraphicsPipelineBuilder gpb;
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);

	for (int i = 0; i < ShaderConvert_Count; i++)
	{
		const bool depth = IsDepthConvertShader(i);

		VkRenderPass rp;
		switch (i)
		{
			case ShaderConvert_RGBA8_TO_16_BITS:
				rp = g_vulkan_context->GetRenderPass(VK_FORMAT_R32G32_UINT, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case ShaderConvert_FLOAT32_TO_32_BITS:
				rp = g_vulkan_context->GetRenderPass(VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case ShaderConvert_DATM_0:
			case ShaderConvert_DATM_1:
				rp = m_date_setup_render_pass;
				break;
			default:
				rp = g_vulkan_context->GetRenderPass(depth ? VK_FORMAT_UNDEFINED : RT_FORMAT, depth ? DEPTH_FORMAT : VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
		}
		if (!rp)
			return false;

		gpb.SetRenderPass(rp, 0);

		if (IsDATMConvertShader(i))
		{
			const VkStencilOpState sos = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 1u, 1u, 1u};
			gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
			gpb.SetStencilState(true, sos, sos);
		}
		else
		{
			gpb.SetDepthState(depth, depth, VK_COMPARE_OP_ALWAYS);
			gpb.SetNoStencilState();
		}

		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([&ps]() { Vulkan::Util::SafeDestroyShaderModule(ps); });
		gpb.SetFragmentShader(ps);

		m_convert[i] = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_convert[i])
			return false;

		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_convert[i], "Convert pipeline %d", i);

		if (swapchain && IsPresentConvertShader(i))
		{
			// compile a present variant too
			gpb.SetRenderPass(m_swap_chain_render_pass, 0);
			m_present[i] = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_present[i])
				return false;

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_present[i], "Convert pipeline %d (Present)", i);
		}

		if (i == ShaderConvert_COPY)
		{
			// compile the variant for setting up hdr rendering
			pxAssert(!m_hdr_no_depth_setup_pipeline && !m_hdr_depth_setup_pipeline);
			gpb.SetRenderPass(GetTFXRenderPass(true, false, true, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE), 0);
			m_hdr_no_depth_setup_pipeline = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_hdr_no_depth_setup_pipeline)
				return false;

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_hdr_no_depth_setup_pipeline, "HDR setup/copy pipeline (no depth)", i);

			gpb.SetRenderPass(GetTFXRenderPass(true, true, true, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE), 0);
			m_hdr_depth_setup_pipeline = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_hdr_depth_setup_pipeline)
				return false;

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_hdr_depth_setup_pipeline, "HDR setup/copy pipeline (depth)", i);
		}
		else if (i == ShaderConvert_MOD_256)
		{
			pxAssert(!m_hdr_no_depth_finish_pipeline && !m_hdr_depth_finish_pipeline);
			gpb.SetRenderPass(GetTFXRenderPass(true, false, false, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE), 0);
			m_hdr_no_depth_finish_pipeline = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_hdr_no_depth_finish_pipeline)
				return false;

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_hdr_no_depth_finish_pipeline, "HDR finish/copy pipeline (no depth)", i);

			gpb.SetRenderPass(GetTFXRenderPass(true, true, false, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE), 0);
			m_hdr_depth_finish_pipeline = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_hdr_depth_finish_pipeline)
				return false;

			Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_hdr_depth_finish_pipeline, "HDR finish/copy pipeline (depth)", i);
		}
	}

	return true;
}

bool GSDeviceVK::CompileInterlacePipelines()
{
	std::optional<std::string> shader = Host::ReadResourceFileToString("vk_interlace.glsl");
	if (!shader)
		return false;

	VkRenderPass rp = g_vulkan_context->GetRenderPass(RT_FORMAT, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([&vs]() { Vulkan::Util::SafeDestroyShaderModule(vs); });

	Vulkan::GraphicsPipelineBuilder gpb;
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_interlace.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);

		m_interlace[i] = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
		Vulkan::Util::SafeDestroyShaderModule(ps);
		if (!m_interlace[i])
			return false;

		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_convert[i], "Interlace pipeline %d", i);
	}

	return true;
}

bool GSDeviceVK::CompileMergePipelines()
{
	std::optional<std::string> shader = Host::ReadResourceFileToString("vk_merge.glsl");
	if (!shader)
		return false;

	VkRenderPass rp = g_vulkan_context->GetRenderPass(RT_FORMAT, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([&vs]() { Vulkan::Util::SafeDestroyShaderModule(vs); });

	Vulkan::GraphicsPipelineBuilder gpb;
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_merge.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);
		gpb.SetBlendAttachment(0, (i > 0),
			VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);

		m_merge[i] = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
		Vulkan::Util::SafeDestroyShaderModule(ps);
		if (!m_merge[i])
			return false;

		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_convert[i], "Merge pipeline %d", i);
	}

	return true;
}

void GSDeviceVK::DestroyResources()
{
	g_vulkan_context->ExecuteCommandBuffer(true);
	if (m_tfx_descriptor_sets[0] != VK_NULL_HANDLE)
		g_vulkan_context->FreeGlobalDescriptorSet(m_tfx_descriptor_sets[0]);

	for (auto& it : m_tfx_pipelines)
		Vulkan::Util::SafeDestroyPipeline(it.second);
	for (auto& it : m_tfx_fragment_shaders)
		Vulkan::Util::SafeDestroyShaderModule(it.second);
	for (auto& it : m_tfx_geometry_shaders)
		Vulkan::Util::SafeDestroyShaderModule(it.second);
	for (auto& it : m_tfx_vertex_shaders)
		Vulkan::Util::SafeDestroyShaderModule(it.second);
	for (VkPipeline& it : m_interlace)
		Vulkan::Util::SafeDestroyPipeline(it);
	for (VkPipeline& it : m_merge)
		Vulkan::Util::SafeDestroyPipeline(it);
	for (VkPipeline& it : m_color_copy)
		Vulkan::Util::SafeDestroyPipeline(it);
	for (VkPipeline& it : m_present)
		Vulkan::Util::SafeDestroyPipeline(it);
	for (VkPipeline& it : m_convert)
		Vulkan::Util::SafeDestroyPipeline(it);
	Vulkan::Util::SafeDestroyPipeline(m_hdr_no_depth_setup_pipeline);
	Vulkan::Util::SafeDestroyPipeline(m_hdr_depth_setup_pipeline);
	Vulkan::Util::SafeDestroyPipeline(m_hdr_no_depth_finish_pipeline);
	Vulkan::Util::SafeDestroyPipeline(m_hdr_depth_finish_pipeline);
	for (auto& it : m_samplers)
		Vulkan::Util::SafeDestroySampler(it.second);

	m_linear_sampler = VK_NULL_HANDLE;
	m_point_sampler = VK_NULL_HANDLE;

	m_utility_color_render_pass_load = VK_NULL_HANDLE;
	m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	m_date_setup_render_pass = VK_NULL_HANDLE;
	m_swap_chain_render_pass = VK_NULL_HANDLE;

	if (m_readback_staging_buffer.IsMapped())
		m_readback_staging_buffer.Unmap();
	m_readback_staging_buffer.Destroy(false);

	m_fragment_uniform_stream_buffer.Destroy(false);
	m_vertex_uniform_stream_buffer.Destroy(false);
	m_index_stream_buffer.Destroy(false);
	m_vertex_stream_buffer.Destroy(false);
	m_texture_upload_buffer.Destroy(false);

	Vulkan::Util::SafeDestroyPipelineLayout(m_tfx_pipeline_layout);
	Vulkan::Util::SafeDestroyDescriptorSetLayout(m_tfx_rt_texture_ds_layout);
	Vulkan::Util::SafeDestroyDescriptorSetLayout(m_tfx_sampler_ds_layout);
	Vulkan::Util::SafeDestroyDescriptorSetLayout(m_tfx_ubo_ds_layout);
	Vulkan::Util::SafeDestroyPipelineLayout(m_utility_pipeline_layout);
	Vulkan::Util::SafeDestroyDescriptorSetLayout(m_utility_ds_layout);

	m_null_texture.Destroy(false);
}

VkShaderModule GSDeviceVK::GetTFXVertexShader(VSSelector sel)
{
	const auto it = m_tfx_vertex_shaders.find(sel.key);
	if (it != m_tfx_vertex_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	AddMacro(ss, "VS_TME", sel.tme);
	AddMacro(ss, "VS_FST", sel.fst);
	AddMacro(ss, "VS_POINT", sel.point);
	AddMacro(ss, "VS_POINT_SIZE", std::max(1, m_upscale_multiplier));
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetVertexShader(ss.str());
	if (mod)
		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), mod, "TFX Vertex %08X", sel.key);

	m_tfx_vertex_shaders.emplace(sel.key, mod);
	return mod;
}

VkShaderModule GSDeviceVK::GetTFXGeometryShader(GSSelector sel)
{
	const auto it = m_tfx_geometry_shaders.find(sel.key);
	if (it != m_tfx_geometry_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, true, false);
	AddMacro(ss, "GS_IIP", sel.iip);
	AddMacro(ss, "GS_PRIM", sel.prim);
	AddMacro(ss, "GS_POINT", sel.point);
	AddMacro(ss, "GS_LINE", sel.line);
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetGeometryShader(ss.str());
	if (mod)
		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), mod, "TFX Geometry %08X", sel.key);

	m_tfx_geometry_shaders.emplace(sel.key, mod);
	return mod;
}

VkShaderModule GSDeviceVK::GetTFXFragmentShader(PSSelector sel)
{
	const auto it = m_tfx_fragment_shaders.find(sel.key);
	if (it != m_tfx_fragment_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	AddMacro(ss, "PS_SCALE_FACTOR", std::max(1, m_upscale_multiplier));
	AddMacro(ss, "PS_FST", sel.fst);
	AddMacro(ss, "PS_WMS", sel.wms);
	AddMacro(ss, "PS_WMT", sel.wmt);
	AddMacro(ss, "PS_FMT", sel.fmt);
	AddMacro(ss, "PS_AEM", sel.aem);
	AddMacro(ss, "PS_TFX", sel.tfx);
	AddMacro(ss, "PS_TCC", sel.tcc);
	AddMacro(ss, "PS_ATST", sel.atst);
	AddMacro(ss, "PS_FOG", sel.fog);
	AddMacro(ss, "PS_CLR1", sel.clr1);
	AddMacro(ss, "PS_FBA", sel.fba);
	AddMacro(ss, "PS_FBMASK", sel.fbmask);
	AddMacro(ss, "PS_LTF", sel.ltf);
	AddMacro(ss, "PS_TCOFFSETHACK", sel.tcoffsethack);
	AddMacro(ss, "PS_POINT_SAMPLER", sel.point_sampler);
	AddMacro(ss, "PS_SHUFFLE", sel.shuffle);
	AddMacro(ss, "PS_READ_BA", sel.read_ba);
	AddMacro(ss, "PS_CHANNEL_FETCH", sel.channel);
	AddMacro(ss, "PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	AddMacro(ss, "PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	AddMacro(ss, "PS_DFMT", sel.dfmt);
	AddMacro(ss, "PS_DEPTH_FMT", sel.depth_fmt);
	AddMacro(ss, "PS_PAL_FMT", sel.fmt >> 2);
	AddMacro(ss, "PS_INVALID_TEX0", sel.invalid_tex0);
	AddMacro(ss, "PS_HDR", sel.hdr);
	AddMacro(ss, "PS_COLCLIP", sel.colclip);
	AddMacro(ss, "PS_BLEND_A", sel.blend_a);
	AddMacro(ss, "PS_BLEND_B", sel.blend_b);
	AddMacro(ss, "PS_BLEND_C", sel.blend_c);
	AddMacro(ss, "PS_BLEND_D", sel.blend_d);
	AddMacro(ss, "PS_PABE", sel.pabe);
	AddMacro(ss, "PS_DITHER", sel.dither);
	AddMacro(ss, "PS_ZCLAMP", sel.zclamp);
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetFragmentShader(ss.str());
	if (mod)
		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), mod, "TFX Fragment %" PRIX64, sel.key);

	m_tfx_fragment_shaders.emplace(sel.key, mod);
	return mod;
}

VkPipeline GSDeviceVK::CreateTFXPipeline(const PipelineSelector& p)
{
	VkShaderModule vs = GetTFXVertexShader(p.vs);
	VkShaderModule gs = p.gs.IsNeeded() ? GetTFXGeometryShader(p.gs) : VK_NULL_HANDLE;
	VkShaderModule fs = GetTFXFragmentShader(p.ps);
	if (vs == VK_NULL_HANDLE || (p.gs.IsNeeded() && gs == VK_NULL_HANDLE) || fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	Vulkan::GraphicsPipelineBuilder gpb;

	// Common state
	gpb.SetPipelineLayout(m_tfx_pipeline_layout);
	gpb.SetRenderPass(GetTFXRenderPass(p.rt, p.ds, p.ps.hdr, p.dss.date, VK_ATTACHMENT_LOAD_OP_LOAD), 0);
	gpb.SetPrimitiveTopology(static_cast<VkPrimitiveTopology>(p.topology));
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);

	// Shaders
	gpb.SetVertexShader(vs);
	if (gs != VK_NULL_HANDLE)
		gpb.SetGeometryShader(gs);
	gpb.SetFragmentShader(fs);

	// IA
	gpb.AddVertexBuffer(0, sizeof(GSVertex));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0); // ST
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UINT, 8); // RGBA
	gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32_SFLOAT, 12); // Q
	gpb.AddVertexAttribute(3, 0, VK_FORMAT_R16G16_UINT, 16); // XY
	gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_UINT, 20); // Z
	gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24); // UV
	gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG

	// DepthStencil
	static const VkCompareOp ztst[] =
		{
			VK_COMPARE_OP_NEVER,
			VK_COMPARE_OP_ALWAYS,
			VK_COMPARE_OP_GREATER_OR_EQUAL,
			VK_COMPARE_OP_GREATER};
	gpb.SetDepthState((p.dss.ztst != ZTST_ALWAYS || p.dss.zwe), p.dss.zwe, ztst[p.dss.ztst]);
	if (p.dss.date)
	{
		const VkStencilOpState sos{
			VK_STENCIL_OP_KEEP, p.dss.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
			VK_COMPARE_OP_EQUAL, 1u, 1u, 1u};
		gpb.SetStencilState(true, sos, sos);
	}

	// Blending
	if (p.bs.abe)
	{
		const HWBlend blend = GetBlend(p.bs.blend_index);
		gpb.SetBlendAttachment(0, true,
			p.bs.accu_blend ? VK_BLEND_FACTOR_ONE : static_cast<VkBlendFactor>(blend.src),
			p.bs.accu_blend ? VK_BLEND_FACTOR_ONE : static_cast<VkBlendFactor>(blend.dst),
			static_cast<VkBlendOp>(blend.op),
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, p.bs.wrgba);
	}
	else
	{
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, p.bs.wrgba);
	}

	VkPipeline pipeline = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true));
	if (pipeline)
	{
		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), pipeline, "TFX Pipeline %08X/%08X/%" PRIX64,
			p.vs.key, p.gs.key, p.ps.key);
	}

	return pipeline;
}

VkPipeline GSDeviceVK::GetTFXPipeline(const PipelineSelector& p)
{
	const auto it = m_tfx_pipelines.find(p);
	if (it != m_tfx_pipelines.end())
		return it->second;

	VkPipeline pipeline = CreateTFXPipeline(p);
	m_tfx_pipelines.emplace(p, pipeline);
	return pipeline;
}

bool GSDeviceVK::BindDrawPipeline(const PipelineSelector& p, u8 afix)
{
	VkPipeline pipeline = GetTFXPipeline(p);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	const float col = float(afix) / 128.0f;
	SetBlendConstants(GSVector4(col));
	SetPipeline(pipeline);

	return ApplyTFXState();
}

void GSDeviceVK::InitializeState()
{
	m_vertex_buffer = m_vertex_stream_buffer.GetBuffer();
	m_vertex_buffer_offset = 0;
	m_index_buffer = m_index_stream_buffer.GetBuffer();
	m_index_buffer_offset = 0;
	m_index_type = VK_INDEX_TYPE_UINT32;
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_pass = VK_NULL_HANDLE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.GetView();

	m_utility_texture = m_null_texture.GetView();

	const SamplerSelector point_selector(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
	const SamplerSelector linear_selector(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
	m_point_sampler = GetSampler(point_selector);
	if (m_point_sampler)
		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_point_sampler, "Point sampler");
	m_linear_sampler = GetSampler(linear_selector);
	if (m_linear_sampler)
		Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_point_sampler, "Linear sampler");

	for (u32 i = 0; i < NUM_TFX_SAMPLERS; i++)
	{
		m_tfx_sampler_sel[i] = point_selector.key;
		m_tfx_samplers[i] = m_point_sampler;
	}

	InvalidateCachedState();
}

bool GSDeviceVK::CreatePersistentDescriptorSets()
{
	const VkDevice dev = g_vulkan_context->GetDevice();
	Vulkan::DescriptorSetUpdateBuilder dsub;

	// Allocate UBO descriptor sets for TFX.
	m_tfx_descriptor_sets[0] = g_vulkan_context->AllocatePersistentDescriptorSet(m_tfx_ubo_ds_layout);
	if (m_tfx_descriptor_sets[0] == VK_NULL_HANDLE)
		return false;
	dsub.AddBufferDescriptorWrite(m_tfx_descriptor_sets[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_vertex_uniform_stream_buffer.GetBuffer(), 0, sizeof(VSConstantBuffer));
	dsub.AddBufferDescriptorWrite(m_tfx_descriptor_sets[0], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_fragment_uniform_stream_buffer.GetBuffer(), 0, sizeof(PSConstantBuffer));
	dsub.Update(dev);
	Vulkan::Util::SetObjectName(dev, m_tfx_descriptor_sets[0], "Persistent TFX UBO set");
	return true;
}

bool GSDeviceVK::CreateReadbackTexture()
{
	const u32 width = 1280 * m_upscale_multiplier;
	const u32 height = 1280 * m_upscale_multiplier;
	const u32 size = width * height * sizeof(u32);

	if (!m_readback_staging_buffer.Create(Vulkan::StagingBuffer::Type::Readback, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
		return false;

	if (!m_readback_staging_buffer.Map())
		return false;

	return true;
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion)
{
	EndRenderPass();
	g_vulkan_context->ExecuteCommandBuffer(wait_for_completion);
	InvalidateCachedState();
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...)
{
	std::va_list ap;
	va_start(ap, reason);
	const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
	va_end(ap);

	Console.Warning("Vulkan: Executing command buffer due to '%s'", reason_str.c_str());
	ExecuteCommandBuffer(wait_for_completion);
}

void GSDeviceVK::ExecuteCommandBufferAndRestartRenderPass(const char* reason)
{
	Console.Warning("Vulkan: Executing command buffer due to '%s'", reason);

	const VkRenderPass render_pass = m_current_render_pass;
	const GSVector4i render_pass_area(m_current_render_pass_area);
	EndRenderPass();
	g_vulkan_context->ExecuteCommandBuffer(false);
	InvalidateCachedState();

	if (render_pass != VK_NULL_HANDLE)
	{
		// rebind framebuffer
		ApplyBaseState(m_dirty_flags, g_vulkan_context->GetCurrentCommandBuffer());
		m_dirty_flags &= ~DIRTY_BASE_STATE;

		// restart render pass
		BeginRenderPass(render_pass, render_pass_area);
	}
}

void GSDeviceVK::InvalidateCachedState()
{
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS | DIRTY_FLAG_TFX_RT_TEXTURE_DS | DIRTY_FLAG_TFX_DYNAMIC_OFFSETS |
					 DIRTY_FLAG_UTILITY_TEXTURE | DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_VERTEX_BUFFER | DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_VIEWPORT |
					 DIRTY_FLAG_SCISSOR | DIRTY_FLAG_PIPELINE;
	if (m_vertex_buffer != VK_NULL_HANDLE)
		m_dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER;
	if (m_index_buffer != VK_NULL_HANDLE)
		m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
	m_current_pipeline_layout = PipelineLayout::Undefined;
	m_tfx_descriptor_sets[1] = VK_NULL_HANDLE;
	m_tfx_descriptor_sets[2] = VK_NULL_HANDLE;
	m_utility_descriptor_set = VK_NULL_HANDLE;
}

void GSDeviceVK::SetVertexBuffer(VkBuffer buffer, VkDeviceSize offset)
{
	if (m_vertex_buffer == buffer && m_vertex_buffer_offset == offset)
		return;

	m_vertex_buffer = buffer;
	m_vertex_buffer_offset = offset;
	m_dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER;
}

void GSDeviceVK::SetIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
	if (m_index_buffer == buffer && m_index_buffer_offset == offset && m_index_type == type)
		return;

	m_index_buffer = buffer;
	m_index_buffer_offset = offset;
	m_index_type = type;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDeviceVK::SetFramebuffer(VkFramebuffer framebuffer)
{
	if (m_current_framebuffer == framebuffer)
		return;

	EndRenderPass();
	m_current_framebuffer = framebuffer;
}

void GSDeviceVK::SetBlendConstants(GSVector4 color)
{
	if ((m_blend_constants == color).alltrue())
		return;

	m_blend_constants = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDeviceVK::PSSetShaderResource(int i, GSTexture* sr)
{
	VkImageView view;
	if (sr)
	{
		GSTextureVK* vkTex = static_cast<GSTextureVK*>(sr);
		if (vkTex->GetTexture().GetLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && InRenderPass())
		{
			// Console.Warning("Ending render pass due to resource transition");
			EndRenderPass();
		}

		vkTex->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkTex->last_frame_used = m_frame;
		view = vkTex->GetView();
	}
	else
	{
		view = m_null_texture.GetView();
	}

	if (m_tfx_textures[i] == view)
		return;

	m_tfx_textures[i] = view;

	m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
}

void GSDeviceVK::PSSetSampler(u32 index, SamplerSelector sel)
{
	if (m_tfx_sampler_sel[index] == sel.key)
		return;

	m_tfx_sampler_sel[index] = sel.key;
	m_tfx_samplers[index] = GetSampler(sel);
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
}

void GSDeviceVK::SetUtilityTexture(GSTexture* tex, VkSampler sampler)
{
	VkImageView view;
	if (tex)
	{
		GSTextureVK* vkTex = static_cast<GSTextureVK*>(tex);
		vkTex->last_frame_used = m_frame;
		vkTex->TransitionToLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		view = vkTex->GetView();
	}
	else
	{
		view = m_null_texture.GetView();
	}

	if (m_utility_texture == view && m_utility_sampler == sampler)
		return;

	m_utility_texture = view;
	m_utility_sampler = sampler;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

void GSDeviceVK::SetUtilityPushConstants(const void* data, u32 size)
{
	vkCmdPushConstants(g_vulkan_context->GetCurrentCommandBuffer(), m_utility_pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void GSDeviceVK::UnbindTexture(VkImageView view)
{
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == view)
		{
			m_tfx_textures[i] = m_null_texture.GetView();
			m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
		}
	}
	if (m_utility_texture == view)
	{
		m_utility_texture = m_null_texture.GetView();
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
}

bool GSDeviceVK::InRenderPass()
{
	return m_current_render_pass != VK_NULL_HANDLE;
}

void GSDeviceVK::BeginRenderPass(VkRenderPass rp, const GSVector4i& rect)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		nullptr,
		m_current_render_pass,
		m_current_framebuffer,
		{{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}},
		0,
		nullptr};

	vkCmdBeginRenderPass(g_vulkan_context->GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const GSVector4& clear_color)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;

	alignas(16) std::array<VkClearValue, 2> cv = {};
	GSVector4::store<true>((void*)cv[0].color.float32, clear_color);

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		nullptr,
		m_current_render_pass,
		m_current_framebuffer,
		{{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}},
		static_cast<u32>(cv.size()),
		cv.data()};

	vkCmdBeginRenderPass(g_vulkan_context->GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

bool GSDeviceVK::CheckRenderPassArea(const GSVector4i& rect)
{
	if (!InRenderPass())
		return false;

	// TODO: Is there a way to do this with GSVector?
	if (rect.left < m_current_render_pass_area.left || rect.top < m_current_render_pass_area.top ||
		rect.right > m_current_render_pass_area.right || rect.bottom > m_current_render_pass_area.bottom)
	{
#ifdef PCSX2_DEVBUILD
		Console.Error("RP check failed: {%d,%d %dx%d} vs {%d,%d %dx%d}", rect.left, rect.top, rect.width(), rect.height(),
			m_current_render_pass_area.left, m_current_render_pass_area.top, m_current_render_pass_area.width(), m_current_render_pass_area.height());
#endif
		return false;
	}

	return true;
}

void GSDeviceVK::EndRenderPass()
{
	if (m_current_render_pass == VK_NULL_HANDLE)
		return;

	vkCmdEndRenderPass(g_vulkan_context->GetCurrentCommandBuffer());

	m_current_render_pass = VK_NULL_HANDLE;
}

void GSDeviceVK::SetViewport(const VkViewport& viewport)
{
	if (std::memcmp(&viewport, &m_viewport, sizeof(VkViewport)) == 0)
		return;

	std::memcpy(&m_viewport, &viewport, sizeof(VkViewport));
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDeviceVK::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDeviceVK::SetPipeline(VkPipeline pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

void GSDeviceVK::SetViewportFromRect(const GSVector4i& rc)
{
	const VkViewport vp{
		m_hack_topleft_offset + static_cast<float>(rc.x),
		m_hack_topleft_offset + static_cast<float>(rc.y),
		static_cast<float>(rc.width()), static_cast<float>(rc.height()),
		0.0f, 1.0f};

	SetViewport(vp);
}

void GSDeviceVK::SetViewportAndScissor(const GSVector4i& rc)
{
	SetViewportFromRect(rc);
	SetScissor(rc);
}

__ri void GSDeviceVK::ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf)
{
	if (flags & DIRTY_FLAG_VERTEX_BUFFER)
		vkCmdBindVertexBuffers(cmdbuf, 0, 1, &m_vertex_buffer, &m_vertex_buffer_offset);

	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		vkCmdBindIndexBuffer(cmdbuf, m_index_buffer, m_index_buffer_offset, m_index_type);

	if (flags & DIRTY_FLAG_PIPELINE)
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline);

	if (flags & DIRTY_FLAG_VIEWPORT)
		vkCmdSetViewport(cmdbuf, 0, 1, &m_viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const VkRect2D vscissor{{m_scissor.x, m_scissor.y}, {static_cast<u32>(m_scissor.width()), static_cast<u32>(m_scissor.height())}};
		vkCmdSetScissor(cmdbuf, 0, 1, &vscissor);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
		vkCmdSetBlendConstants(cmdbuf, m_blend_constants.v);
}

bool GSDeviceVK::ApplyTFXState()
{
	if (m_current_pipeline_layout == PipelineLayout::TFX && m_dirty_flags == 0)
		return true;

	const VkDevice dev = g_vulkan_context->GetDevice();
	const VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_TFX_STATE;

	Vulkan::DescriptorSetUpdateBuilder dsub;

	u32 dirty_descriptor_set_start = NUM_TFX_DESCRIPTOR_SETS;
	u32 dirty_descriptor_set_end = 0;

	if (flags & DIRTY_FLAG_TFX_DYNAMIC_OFFSETS)
	{
		dirty_descriptor_set_start = 0;
	}

	if ((flags & DIRTY_FLAG_TFX_SAMPLERS_DS) || m_tfx_descriptor_sets[1] == VK_NULL_HANDLE)
	{
		VkDescriptorSet ds = g_vulkan_context->AllocateDescriptorSet(m_tfx_sampler_ds_layout);
		if (ds == VK_NULL_HANDLE)
		{
			ExecuteCommandBufferAndRestartRenderPass("Ran out of TFX texture descriptors");
			return ApplyTFXState();
		}

		dsub.AddCombinedImageSamplerDescriptorWrites(ds, 0, m_tfx_textures.data(), m_tfx_samplers.data(), NUM_TFX_SAMPLERS);
		dsub.Update(dev);

		m_tfx_descriptor_sets[1] = ds;
		dirty_descriptor_set_start = std::min(dirty_descriptor_set_start, 1u);
		dirty_descriptor_set_end = 1u;
	}

	if ((flags & DIRTY_FLAG_TFX_RT_TEXTURE_DS) || m_tfx_descriptor_sets[2] == VK_NULL_HANDLE)
	{
		VkDescriptorSet ds = g_vulkan_context->AllocateDescriptorSet(m_tfx_rt_texture_ds_layout);
		if (ds == VK_NULL_HANDLE)
		{
			ExecuteCommandBufferAndRestartRenderPass("Ran out of TFX sampler descriptors");
			return ApplyTFXState();
		}

		dsub.AddImageDescriptorWrites(ds, 0, &m_tfx_textures[NUM_TFX_SAMPLERS], NUM_TFX_RT_TEXTURES);
		dsub.Update(dev);

		m_tfx_descriptor_sets[2] = ds;
		dirty_descriptor_set_start = std::min(dirty_descriptor_set_start, 2u);
		dirty_descriptor_set_end = 2u;
	}

	if (m_current_pipeline_layout != PipelineLayout::TFX)
	{
		m_current_pipeline_layout = PipelineLayout::TFX;

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout,
			0, NUM_TFX_DESCRIPTOR_SETS, m_tfx_descriptor_sets.data(), NUM_TFX_DYNAMIC_OFFSETS, m_tfx_dynamic_offsets.data());
	}
	else if (dirty_descriptor_set_start <= dirty_descriptor_set_end)
	{
		u32 dynamic_count;
		const u32* dynamic_offsets;
		if (dirty_descriptor_set_start == 0)
		{
			dynamic_count = NUM_TFX_DYNAMIC_OFFSETS;
			dynamic_offsets = m_tfx_dynamic_offsets.data();
		}
		else
		{
			dynamic_count = 0;
			dynamic_offsets = nullptr;
		}

		const u32 count = dirty_descriptor_set_end - dirty_descriptor_set_start + 1;

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout,
			dirty_descriptor_set_start, count, &m_tfx_descriptor_sets[dirty_descriptor_set_start],
			dynamic_count, dynamic_offsets);
	}


	ApplyBaseState(flags, cmdbuf);
	return true;
}

bool GSDeviceVK::ApplyUtilityState()
{
	if (m_current_pipeline_layout == PipelineLayout::Utility && m_dirty_flags == 0)
		return true;

	const VkDevice dev = g_vulkan_context->GetDevice();
	const VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_UTILITY_STATE;

	bool rebind = (m_current_pipeline_layout != PipelineLayout::Utility);

	if ((flags & DIRTY_FLAG_UTILITY_TEXTURE) || m_utility_descriptor_set == VK_NULL_HANDLE)
	{
		m_utility_descriptor_set = g_vulkan_context->AllocateDescriptorSet(m_utility_ds_layout);
		if (m_utility_descriptor_set == VK_NULL_HANDLE)
		{
			ExecuteCommandBufferAndRestartRenderPass("Ran out of utility descriptors");
			return ApplyTFXState();
		}

		Vulkan::DescriptorSetUpdateBuilder dsub;
		dsub.AddCombinedImageSamplerDescriptorWrite(m_utility_descriptor_set, 0, m_utility_texture, m_utility_sampler);
		dsub.Update(dev);
		rebind = true;
	}

	if (rebind)
	{
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_utility_pipeline_layout,
			0, 1, &m_utility_descriptor_set, 0, nullptr);
	}

	m_current_pipeline_layout = PipelineLayout::Utility;

	ApplyBaseState(flags, cmdbuf);
	return true;
}
