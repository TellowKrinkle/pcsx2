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
#include "GSMetalCPPAccessible.h"
#include "GSDeviceMTL.h"
#include "GSTextureMTL.h"

#if ! __has_feature(objc_arc)
	#error "Compile this with -fobjc-arc"
#endif

#ifdef __APPLE__
#include "GSMTLSharedHeader.h"

GSDevice* makeGSDeviceMTL()
{
	return new GSDeviceMTL();
}

std::vector<std::string> getMTLAdapters(size_t* default_adapter_idx)
{
	if (default_adapter_idx)
		*default_adapter_idx = 0;
	std::vector<std::string> ret;
	@autoreleasepool
	{
		id<MTLDevice> default_adapter = MTLCreateSystemDefaultDevice();
		for (id<MTLDevice> dev in MTLCopyAllDevices())
		{
			if (dev == default_adapter && default_adapter_idx)
				*default_adapter_idx = ret.size();
			ret.push_back([[dev name] UTF8String]);
		}
	}
	return ret;
}

static MTLScissorRect makeScissorRect(NSUInteger x, NSUInteger y, NSUInteger width, NSUInteger height)
{
	MTLScissorRect rect;
	rect.x = x;
	rect.y = y;
	rect.width = width;
	rect.height = height;
	return rect;
}

bool GSDeviceMTL::UsageTracker::PrepareForAllocation(u64 last_draw, size_t amt)
{
	auto removeme = std::find_if(m_usage.begin(), m_usage.end(), [last_draw](UsageEntry usage){ return usage.drawno > last_draw; });
	if (removeme != m_usage.begin())
		m_usage.erase(m_usage.begin(), removeme);

	bool still_in_use = false;
	bool needs_wrap = m_pos + amt > m_size;
	if (!m_usage.empty())
	{
		size_t used = m_usage.front().pos;
		if (needs_wrap)
			still_in_use = used >= m_pos || used < amt;
		else
			still_in_use = used >= m_pos && used < m_pos + amt;
	}
	if (needs_wrap)
		m_pos = 0;

	return still_in_use || amt > m_size;
}

size_t GSDeviceMTL::UsageTracker::Allocate(u64 current_draw, size_t amt)
{
	if (m_usage.empty() || m_usage.back().drawno != current_draw)
		m_usage.push_back({current_draw, m_pos});
	size_t ret = m_pos;
	m_pos += amt;
	return ret;
}

void GSDeviceMTL::UsageTracker::Reset(size_t new_size)
{
	m_usage.clear();
	m_size = new_size;
	m_pos = 0;
}

GSDeviceMTL::OutlivesDeviceObj::OutlivesDeviceObj(GSDeviceMTL* dev)
	: backref(dev)
	, gpu_work_sema(dispatch_semaphore_create(3))
{
}

GSDeviceMTL::GSDeviceMTL()
	: m_outlive(std::make_shared<OutlivesDeviceObj>(this))
	, m_dev(nil)
{
	m_mipmap = theApp.GetConfigI("mipmap");
	if (theApp.GetConfigB("UserHacks"))
		m_filter = static_cast<TriFiltering>(theApp.GetConfigI("UserHacks_TriFilter"));
	else
		m_filter = TriFiltering::None;
}

static void GSDeviceMTLCleanup(NSView* view, CAMetalLayer* layer)
{
	if (![NSThread isMainThread])
	{
		dispatch_async(dispatch_get_main_queue(), ^{ GSDeviceMTLCleanup(view, layer); });
		return;
	}
	if ([view layer] == layer)
	{
		[view setLayer:nil];
		[view setWantsLayer:NO];
	}
}

GSDeviceMTL::~GSDeviceMTL()
{ @autoreleasepool {
	m_drawable_fetcher.Stop();
	FlushEncoders();
	GSDeviceMTLCleanup(m_view, m_layer);
	std::lock_guard<std::mutex> guard(m_outlive->mtx);
	m_outlive->backref = nullptr;
}}

GSDeviceMTL::Map GSDeviceMTL::Allocate(UploadBuffer& buffer, size_t amt)
{
	amt = (amt + 31) & ~31ull;
	u64 last_draw = m_last_finished_draw.load(std::memory_order_acquire);
	bool needs_new = buffer.usage.PrepareForAllocation(last_draw, amt);
	if (unlikely(needs_new))
	{
		// Orphan buffer
		size_t newsize = std::max<size_t>(buffer.usage.Size() * 2, 4096);
		while (newsize < amt)
			newsize *= 2;
		MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
		buffer.mtlbuffer = [m_dev newBufferWithLength:newsize options:options];
		pxAssertRel(buffer.mtlbuffer, "Failed to allocate MTLBuffer (out of memory?)");
		buffer.buffer = buffer.mtlbuffer.contents;
		buffer.usage.Reset(newsize);
	}

	size_t pos = buffer.usage.Allocate(m_current_draw, amt);

	Map ret = {buffer.mtlbuffer, pos, reinterpret_cast<char*>(buffer.buffer) + pos};
	ASSERT(pos <= buffer.usage.Size() && "Previous code should have guaranteed there was enough space");
	return ret;
}

/// Allocate space in the given buffer for use with the given render command encoder
GSDeviceMTL::Map GSDeviceMTL::Allocate(BufferPair& buffer, size_t amt)
{
	amt = (amt + 31) & ~31ull;
	u64 last_draw = m_last_finished_draw.load(std::memory_order_acquire);
	size_t base_pos = buffer.usage.Pos();
	bool needs_new = buffer.usage.PrepareForAllocation(last_draw, amt);
	bool needs_upload = needs_new || buffer.usage.Pos() == 0;
	if (!m_unified_memory && needs_upload)
	{
		if (base_pos != buffer.last_upload)
		{
			id<MTLBlitCommandEncoder> enc = GetVertexUploadEncoder();
			[enc copyFromBuffer:buffer.cpubuffer
			       sourceOffset:buffer.last_upload
			           toBuffer:buffer.gpubuffer
			  destinationOffset:buffer.last_upload
			               size:base_pos - buffer.last_upload];
		}
		buffer.last_upload = 0;
	}
	if (unlikely(needs_new))
	{
		// Orphan buffer
		size_t newsize = std::max<size_t>(buffer.usage.Size() * 2, 4096);
		while (newsize < amt)
			newsize *= 2;
		MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
		buffer.cpubuffer = [m_dev newBufferWithLength:newsize options:options];
		pxAssertRel(buffer.cpubuffer, "Failed to allocate MTLBuffer (out of memory?)");
		buffer.buffer = buffer.cpubuffer.contents;
		buffer.usage.Reset(newsize);
		if (!m_unified_memory)
		{
			options = MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked;
			buffer.gpubuffer = [m_dev newBufferWithLength:newsize options:options];
			pxAssertRel(buffer.gpubuffer, "Failed to allocate MTLBuffer (out of memory?)");
		}
	}

	size_t pos = buffer.usage.Allocate(m_current_draw, amt);
	Map ret = {nil, pos, reinterpret_cast<char*>(buffer.buffer) + pos};
	ret.gpu_buffer = m_unified_memory ? buffer.cpubuffer : buffer.gpubuffer;
	ASSERT(pos <= buffer.usage.Size() && "Previous code should have guaranteed there was enough space");
	return ret;
}

void GSDeviceMTL::Sync(BufferPair& buffer)
{
	if (m_unified_memory || buffer.usage.Pos() == buffer.last_upload)
		return;

	id<MTLBlitCommandEncoder> enc = GetVertexUploadEncoder();
	[enc copyFromBuffer:buffer.cpubuffer
	       sourceOffset:buffer.last_upload
	           toBuffer:buffer.gpubuffer
	  destinationOffset:buffer.last_upload
	               size:buffer.usage.Pos() - buffer.last_upload];
	[enc updateFence:m_draw_sync_fence];
	buffer.last_upload = buffer.usage.Pos();
}

id<MTLBlitCommandEncoder> GSDeviceMTL::GetTextureUploadEncoder()
{
	if (!m_texture_upload_cmdbuf)
	{
		m_texture_upload_cmdbuf = [m_queue commandBuffer];
		m_texture_upload_encoder = [m_texture_upload_cmdbuf blitCommandEncoder];
		pxAssertRel(m_texture_upload_encoder, "Failed to create texture upload encoder!");
		[m_texture_upload_cmdbuf setLabel:@"Texture Upload"];
	}
	return m_texture_upload_encoder;
}

id<MTLBlitCommandEncoder> GSDeviceMTL::GetLateTextureUploadEncoder()
{
	if (!m_late_texture_upload_encoder)
	{
		EndRenderPass();
		m_late_texture_upload_encoder = [GetRenderCmdBuf() blitCommandEncoder];
		pxAssertRel(m_late_texture_upload_encoder, "Failed to create late texture upload encoder!");
		[m_late_texture_upload_encoder setLabel:@"Late Texture Upload"];
		if (!m_unified_memory)
			[m_late_texture_upload_encoder waitForFence:m_draw_sync_fence];
	}
	return m_late_texture_upload_encoder;
}

id<MTLBlitCommandEncoder> GSDeviceMTL::GetVertexUploadEncoder()
{
	if (!m_vertex_upload_cmdbuf)
	{
		m_vertex_upload_cmdbuf = [m_queue commandBuffer];
		m_vertex_upload_encoder = [m_vertex_upload_cmdbuf blitCommandEncoder];
		pxAssertRel(m_vertex_upload_encoder, "Failed to create vertex upload encoder!");
		[m_vertex_upload_cmdbuf setLabel:@"Vertex Upload"];
	}
	return m_vertex_upload_encoder;
}

/// Get the draw command buffer, creating a new one if it doesn't exist
id<MTLCommandBuffer> GSDeviceMTL::GetRenderCmdBuf()
{
	if (!m_current_render_cmdbuf)
	{
		m_current_render_cmdbuf = [m_queue commandBuffer];
		pxAssertRel(m_current_render_cmdbuf, "Failed to create draw command buffer!");
		[m_current_render_cmdbuf setLabel:@"Draw"];
	}
	return m_current_render_cmdbuf;
}

void GSDeviceMTL::FlushEncoders()
{
	if (!m_current_render_cmdbuf)
		return;
	EndRenderPass();
	Sync(m_vertex_upload_buf);
	if (m_unified_memory)
	{
		ASSERT(!m_vertex_upload_cmdbuf && "Should never be used!");
	}
	else if (m_vertex_upload_cmdbuf)
	{
		[m_vertex_upload_encoder endEncoding];
		[m_vertex_upload_cmdbuf commit];
		m_vertex_upload_encoder = nil;
		m_vertex_upload_cmdbuf = nil;
	}
	if (m_texture_upload_cmdbuf)
	{
		[m_texture_upload_encoder endEncoding];
		[m_texture_upload_cmdbuf commit];
		m_texture_upload_encoder = nil;
		m_texture_upload_cmdbuf = nil;
	}
	if (m_late_texture_upload_encoder)
	{
		[m_late_texture_upload_encoder endEncoding];
		m_late_texture_upload_encoder = nil;
	}
	[m_current_render_cmdbuf addCompletedHandler:[obj = m_outlive, draw = m_current_draw](id<MTLCommandBuffer> buf)
	{
		std::lock_guard<std::mutex> guard(obj->mtx);
		if (GSDeviceMTL* dev = obj->backref)
		{
			// We can do the update non-atomically because we only ever update under the lock
			u64 newval = std::max(draw, dev->m_last_finished_draw.load(std::memory_order_relaxed));
			dev->m_last_finished_draw.store(newval, std::memory_order_release);
		}
	}];
	[m_current_render_cmdbuf commit];
	m_current_render_cmdbuf = nil;
	m_current_draw++;
}

void GSDeviceMTL::EndRenderPass()
{
	if (m_current_render.encoder)
	{
		[m_current_render.encoder endEncoding];
		m_current_render = MainRenderEncoder();
	}
}

GSDeviceMTL::MainRenderEncoder& GSDeviceMTL::BeginRenderPass(GSTexture* color, MTLLoadAction color_load, GSTexture* depth, MTLLoadAction depth_load, GSTexture* stencil, MTLLoadAction stencil_load)
{
	GSTextureMTL* mc = static_cast<GSTextureMTL*>(color);
	GSTextureMTL* md = static_cast<GSTextureMTL*>(depth);
	GSTextureMTL* ms = static_cast<GSTextureMTL*>(stencil);
	bool needs_new = color   != m_current_render.color_target
	              || depth   != m_current_render.depth_target
	              || stencil != m_current_render.stencil_target;
	GSVector4 color_clear;
	float depth_clear;
	int stencil_clear;
	bool needs_color_clear = false;
	bool needs_depth_clear = false;
	bool needs_stencil_clear = false;
	if (mc) needs_color_clear   = mc->GetResetNeedsColorClear(color_clear);
	if (md) needs_depth_clear   = md->GetResetNeedsDepthClear(depth_clear);
	if (ms) needs_stencil_clear = ms->GetResetNeedsStencilClear(stencil_clear);
	if (needs_color_clear   && color_load   != MTLLoadActionDontCare) color_load   = MTLLoadActionClear;
	if (needs_depth_clear   && depth_load   != MTLLoadActionDontCare) depth_load   = MTLLoadActionClear;
	if (needs_stencil_clear && stencil_load != MTLLoadActionDontCare) stencil_load = MTLLoadActionClear;
	needs_new |= mc && color_load   == MTLLoadActionClear;
	needs_new |= md && depth_load   == MTLLoadActionClear;
	needs_new |= ms && stencil_load == MTLLoadActionClear;

	if (!needs_new)
		return m_current_render;

	if (m_late_texture_upload_encoder)
	{
		[m_late_texture_upload_encoder endEncoding];
		m_late_texture_upload_encoder = nullptr;
	}

	int idx = 0;
	if (mc) idx |= 1;
	if (md) idx |= 2;
	if (ms) idx |= 4;

	MTLRenderPassDescriptor* desc = m_render_pass_desc[idx];
	if (mc)
	{
		mc->m_last_write = m_current_draw;
		desc.colorAttachments[0].texture = mc->GetTexture();
		if (color_load == MTLLoadActionClear)
			desc.colorAttachments[0].clearColor = MTLClearColorMake(color_clear.r, color_clear.g, color_clear.b, color_clear.a);
		desc.colorAttachments[0].loadAction = color_load;
	}
	if (md)
	{
		md->m_last_write = m_current_draw;
		desc.depthAttachment.texture = md->GetTexture();
		if (depth_load == MTLLoadActionClear)
			desc.depthAttachment.clearDepth = depth_clear;
		desc.depthAttachment.loadAction = depth_load;
	}
	if (ms)
	{
		ms->m_last_write = m_current_draw;
		desc.stencilAttachment.texture = ms->GetTexture();
		if (stencil_load == MTLLoadActionClear)
			desc.stencilAttachment.clearStencil = stencil_clear;
		desc.stencilAttachment.loadAction = stencil_load;
	}

	EndRenderPass();
	m_current_render.encoder = [GetRenderCmdBuf() renderCommandEncoderWithDescriptor:desc];
	if (!m_unified_memory)
		[m_current_render.encoder waitForFence:m_draw_sync_fence
		                          beforeStages:MTLRenderStageVertex];
	m_current_render.color_target = color;
	m_current_render.depth_target = depth;
	m_current_render.stencil_target = stencil;
	pxAssertRel(m_current_render.encoder, "Failed to create render encoder!");
	return m_current_render;
}

GSTexture* GSDeviceMTL::CreateSurface(GSTexture::Type type, int w, int h, GSTexture::Format format)
{ @autoreleasepool {
	int layers = 1;

	if (m_mipmap && format == GSTexture::Format::Color && type == GSTexture::Type::Texture)
		layers = (int)log2(std::max(w,h));

	MTLPixelFormat fmt = GetPixelFormat(format);
	if (format == GSTexture::Format::Backbuffer || format == GSTexture::Format::Invalid)
	{
		fmt = MTLPixelFormatInvalid;
		pxAssertRel(0, "Can't create surface of this format!");
	}

	MTLTextureDescriptor* desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:fmt
		                             width:std::max(1, std::min(w, m_max_texsize))
		                            height:std::max(1, std::min(h, m_max_texsize))
		                         mipmapped:layers > 1];

	if (layers > 1)
		[desc setMipmapLevelCount:layers];

	[desc setStorageMode:MTLStorageModePrivate];
	switch (type)
	{
		case GSTexture::Type::Texture:
			[desc setUsage:MTLTextureUsageShaderRead];
			break;
		case GSTexture::Type::Offscreen:
			[desc setUsage:MTLTextureUsageRenderTarget];
			break;
		default:
			[desc setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	}

	id<MTLTexture> tex = [m_dev newTextureWithDescriptor:desc];
	if (tex)
	{
		GSTextureMTL* t = new GSTextureMTL(this, tex, type, format);
		switch (type)
		{
			case GSTexture::Type::RenderTarget:
				ClearRenderTarget(t, 0);
				break;
			case GSTexture::Type::DepthStencil:
				ClearDepth(t);
				break;
			default:
				break;
		}
		return t;
	}
	else
	{
		return nullptr;
	}
}}

GSTexture* GSDeviceMTL::FetchSurface(GSTexture::Type type, int w, int h, GSTexture::Format format)
{
	return GSDevice::FetchSurface(type, w, h, format);
}

void GSDeviceMTL::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, const GSVector4& c)
{ @autoreleasepool {
	id<MTLCommandBuffer> cmdbuf = GetRenderCmdBuf();
	GSScopedDebugGroupMTL dbg(cmdbuf, @"DoMerge");

	GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;

	ClearRenderTarget(dTex, c);

	vector_float4 cb_c = { c.r, c.g, c.b, c.a };
	GSMTLConvertPSUniform cb_yuv = {};
	cb_yuv.emoda = EXTBUF.EMODA;
	cb_yuv.emodc = EXTBUF.EMODC;

	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		StretchRect(sTex[1], sRect[1], dTex, dRect[1], ShaderConvert::COPY);
	}

	// Save 2nd output
	if (feedback_write_2) // FIXME I'm not sure dRect[1] is always correct
		DoStretchRect(dTex, full_r, sTex[2], dRect[1], m_convert_pipeline[static_cast<int>(ShaderConvert::YUV)], true, LoadAction::DontCareIfFull, &cb_yuv, sizeof(cb_yuv));

	if (feedback_write_2_but_blend_bg)
		ClearRenderTarget(dTex, c);

	if (sTex[0])
	{
		if (PMODE.AMOD == 1)
		{
			// TODO: OpenGL says keep the alpha from the 2nd output but then sets something that gets overwritten by every StretchRect call...
		}

		// 1st output is enabled. It must be blended
		if (PMODE.MMOD == 1)
		{
			// Blend with a constant alpha
			DoStretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge_pipeline[1], true, LoadAction::Load, &cb_c, sizeof(cb_c));
		}
		else
		{
			// Blend with 2 * input alpha
			DoStretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge_pipeline[0], true, LoadAction::Load, nullptr, 0);
		}
	}

	if (feedback_write_1) // FIXME I'm not sure dRect[0] is always correct
		StretchRect(dTex, full_r, sTex[2], dRect[0], ShaderConvert::YUV);
}}

void GSDeviceMTL::DoInterlace(GSTexture* sTex, GSTexture* dTex, int shader, bool linear, float yoffset)
{ @autoreleasepool {
	id<MTLCommandBuffer> cmdbuf = GetRenderCmdBuf();
	GSScopedDebugGroupMTL dbg(cmdbuf, @"DoInterlace");

	GSVector4 s = GSVector4(dTex->GetSize());

	GSVector4 sRect(0, 0, 1, 1);
	GSVector4 dRect(0.f, yoffset, s.x, s.y + yoffset);

	GSMTLInterlacePSUniform cb = {};
	cb.ZrH = {0, 1.f / s.y};
	cb.hH = s.y / 2;

	DoStretchRect(sTex, sRect, dTex, dRect, m_interlace_pipeline[shader], linear, shader > 1 ? LoadAction::DontCareIfFull : LoadAction::Load, &cb, sizeof(cb));
}}

void GSDeviceMTL::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	// TODO: Implement
}

void GSDeviceMTL::DoShadeBoost(GSTexture* sTex, GSTexture* dTex)
{
	// TODO: Implement
}

void GSDeviceMTL::DoExternalFX(GSTexture* sTex, GSTexture* dTex)
{
	// TODO: Implement
}

u16 GSDeviceMTL::ConvertBlendEnum(u16 generic)
{
	switch (generic)
	{
		case SRC_COLOR       : return MTLBlendFactorSourceColor;
		case INV_SRC_COLOR   : return MTLBlendFactorOneMinusSourceColor;
		case DST_COLOR       : return MTLBlendFactorDestinationColor;
		case INV_DST_COLOR   : return MTLBlendFactorOneMinusBlendColor;
		case SRC1_COLOR      : return MTLBlendFactorSource1Color;
		case INV_SRC1_COLOR  : return MTLBlendFactorOneMinusSource1Color;
		case SRC_ALPHA       : return MTLBlendFactorSourceAlpha;
		case INV_SRC_ALPHA   : return MTLBlendFactorOneMinusSourceAlpha;
		case DST_ALPHA       : return MTLBlendFactorDestinationAlpha;
		case INV_DST_ALPHA   : return MTLBlendFactorOneMinusDestinationAlpha;
		case SRC1_ALPHA      : return MTLBlendFactorSource1Alpha;
		case INV_SRC1_ALPHA  : return MTLBlendFactorOneMinusSource1Alpha;
		case CONST_COLOR     : return MTLBlendFactorBlendColor;
		case INV_CONST_COLOR : return MTLBlendFactorOneMinusBlendColor;
		case CONST_ONE       : return MTLBlendFactorOne;
		case CONST_ZERO      : return MTLBlendFactorZero;
		case OP_ADD          : return MTLBlendOperationAdd;
		case OP_SUBTRACT     : return MTLBlendOperationSubtract;
		case OP_REV_SUBTRACT : return MTLBlendOperationReverseSubtract;
		default              : ASSERT(0); return 0;
	}
}

MTLPixelFormat GSDeviceMTL::GetPixelFormat(GSTexture::Format format)
{
	switch (format)
	{
		case GSTexture::Format::Int32:        return MTLPixelFormatR32Sint;
		case GSTexture::Format::UInt32:       return MTLPixelFormatR32Uint;
		case GSTexture::Format::UInt16:       return MTLPixelFormatR16Uint;
		case GSTexture::Format::UNorm8:       return MTLPixelFormatA8Unorm;
		case GSTexture::Format::Color:        return MTLPixelFormatRGBA8Unorm;
		case GSTexture::Format::FloatColor:   return MTLPixelFormatRGBA32Float;
		case GSTexture::Format::DepthStencil: return MTLPixelFormatDepth32Float_Stencil8;
		case GSTexture::Format::Backbuffer:   return [m_layer pixelFormat];
		case GSTexture::Format::Invalid:      return MTLPixelFormatInvalid;
	}
}

id<MTLFunction> GSDeviceMTL::LoadShader(NSString* name)
{
	NSError* err = nil;
	id<MTLFunction> fn = [m_shaders newFunctionWithName:name constantValues:m_fn_constants error:&err];
	if (unlikely(err))
	{
		NSString* msg = [NSString stringWithFormat:@"Failed to load shader %@: %@", name, [err localizedDescription]];
		throw std::runtime_error([msg UTF8String]);
	}
	return fn;
}

void GSDeviceMTL::InitWindow(const WindowInfo& wi)
{
	ASSERT([NSThread isMainThread]);
	m_view = (__bridge NSView*)wi.window_handle;
	m_layer = [CAMetalLayer layer];
	[m_view setWantsLayer:YES];
	[m_view setLayer:m_layer];
	[m_layer setDevice:m_dev];
}

id<MTLRenderPipelineState> GSDeviceMTL::MakePipeline(MTLRenderPipelineDescriptor* desc, id<MTLFunction> vertex, id<MTLFunction> fragment, NSString* name)
{
	[desc setLabel:name];
	[desc setVertexFunction:vertex];
	[desc setFragmentFunction:fragment];
	NSError* err;
	id<MTLRenderPipelineState> res = [m_dev newRenderPipelineStateWithDescriptor:desc error:&err];
	if (err)
		throw std::runtime_error([[err localizedDescription] UTF8String]);
	return res;
}

static void applyAttribute(MTLVertexDescriptor* desc, NSUInteger idx, MTLVertexFormat fmt, NSUInteger offset, NSUInteger buffer_index)
{
	MTLVertexAttributeDescriptor* attrs = desc.attributes[idx];
	attrs.format = fmt;
	attrs.offset = offset;
	attrs.bufferIndex = buffer_index;
}

static void setFnConstantB(MTLFunctionConstantValues* fc, bool value, GSMTLFnConstants constant)
{
	[fc setConstantValue:&value type:MTLDataTypeBool atIndex:constant];
}

static void setFnConstantI(MTLFunctionConstantValues* fc, unsigned int value, GSMTLFnConstants constant)
{
	[fc setConstantValue:&value type:MTLDataTypeUInt atIndex:constant];
}

bool GSDeviceMTL::Create(const WindowInfo& wi)
{ @autoreleasepool {
	int adapter_idx = theApp.GetConfigI("adapter_index");
	NSArray<id<MTLDevice>>* devs = MTLCopyAllDevices();
	if (adapter_idx < 0 || adapter_idx >= static_cast<int>([devs count]))
	{
		m_dev = MTLCreateSystemDefaultDevice();
		if (!m_dev)
		{
			Console.Error("Metal: No supported devices available!");
			return false;
		}
		if (adapter_idx >= 0)
			Console.Warning("Metal: Device %d requested but only %zd available, using default", adapter_idx, [devs count]);
	}
	else
	{
		m_dev = devs[adapter_idx];
	}
	Console.WriteLn("Metal: Rendering with %s", [[m_dev name] UTF8String]);

	if (char* env = getenv("MTL_CAPTURE"))
	{
		m_capture_frame = atoi(env);
		if (m_capture_frame)
			Console.WriteLn("Metal: Will capture frame %d", m_capture_frame);
		else
			Console.Warning("Metal: Failed to parse capture frame!");
	}
	else
	{
		m_capture_frame = 0;
	}

	if ([NSThread isMainThread])
		InitWindow(wi);
	else
		dispatch_sync(dispatch_get_main_queue(), [this, &wi]{ InitWindow(wi); });

	Reset(wi.surface_width, wi.surface_height);
	m_drawable_fetcher.Start(m_layer);

	m_features.geometry_shader = false;
	m_features.image_load_store = false;
	m_features.texture_barrier = true;

	if (char* env = getenv("MTL_UNIFIED_MEMORY"))
		m_unified_memory = env[0] == '1' || env[0] == 'y' || env[0] == 'Y';
	else if (@available(macOS 10.15, iOS 13.0, *))
		m_unified_memory = [m_dev hasUnifiedMemory];
	else
		m_unified_memory = false;

	m_max_texsize = 8192;
	if ([m_dev supportsFeatureSet:MTLFeatureSet_macOS_GPUFamily1_v1])
		m_max_texsize = 16384;
	if (@available(macOS 10.15, iOS 13.0, *))
		if ([m_dev supportsFamily:MTLGPUFamilyApple3])
			m_max_texsize = 16384;

	if (!(m_dev && m_layer && m_view))
		return false;

	// Init OSD Font
	const GSVector2i tex_font = m_osd.get_texture_font_size();
	m_font.reset(static_cast<GSTextureMTL*>(CreateTexture(tex_font.x, tex_font.y, GSTexture::Format::UNorm8)));

	try
	{
		// Init metal stuff
		m_queue = [m_dev newCommandQueue];
		m_draw_sync_fence = [m_dev newFence];
		m_shaders = [m_dev newDefaultLibrary];

		m_fn_constants = [MTLFunctionConstantValues new];
		u8 upscale = std::max(1, theApp.GetConfigI("upscale_multiplier"));
		vector_uchar2 upscale2 = vector2(upscale, upscale);
		[m_fn_constants setConstantValue:&upscale2 type:MTLDataTypeUChar2 atIndex:GSMTLConstantIndex_SCALING_FACTOR];

		m_hw_vertex = [MTLVertexDescriptor new];
		m_hw_vertex.layouts[GSMTLBufferIndexVertices].stride = sizeof(GSVertex);
		applyAttribute(m_hw_vertex, GSMTLAttributeIndexST, MTLVertexFormatFloat2,           offsetof(GSVertex, ST),      GSMTLBufferIndexVertices);
		applyAttribute(m_hw_vertex, GSMTLAttributeIndexC,  MTLVertexFormatUChar4,           offsetof(GSVertex, RGBAQ.R), GSMTLBufferIndexVertices);
		applyAttribute(m_hw_vertex, GSMTLAttributeIndexQ,  MTLVertexFormatFloat,            offsetof(GSVertex, RGBAQ.Q), GSMTLBufferIndexVertices);
		applyAttribute(m_hw_vertex, GSMTLAttributeIndexXY, MTLVertexFormatUShort2,          offsetof(GSVertex, XYZ.X),   GSMTLBufferIndexVertices);
		applyAttribute(m_hw_vertex, GSMTLAttributeIndexZ,  MTLVertexFormatUInt,             offsetof(GSVertex, XYZ.Z),   GSMTLBufferIndexVertices);
		applyAttribute(m_hw_vertex, GSMTLAttributeIndexUV, MTLVertexFormatUShort2,          offsetof(GSVertex, UV),      GSMTLBufferIndexVertices);
		applyAttribute(m_hw_vertex, GSMTLAttributeIndexF,  MTLVertexFormatUChar4Normalized, offsetof(GSVertex, FOG),     GSMTLBufferIndexVertices);

		for (auto& desc : m_render_pass_desc)
		{
			desc = [MTLRenderPassDescriptor new];
			desc.depthAttachment.storeAction = MTLStoreActionStore;
			desc.stencilAttachment.storeAction = MTLStoreActionStore;
		}

		// Init samplers
		MTLSamplerDescriptor* sdesc = [MTLSamplerDescriptor new];
		const int anisotropy = theApp.GetConfigI("MaxAnisotropy");
		for (size_t i = 0; i < std::size(m_sampler_hw); i++)
		{
			GSHWDrawConfig::SamplerSelector sel;
			sel.key = i;
			sdesc.minFilter = sel.biln ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
			sdesc.magFilter = sel.biln ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
			switch (static_cast<GS_MIN_FILTER>(sel.triln))
			{
				case GS_MIN_FILTER::Nearest:
				case GS_MIN_FILTER::Linear:
					sdesc.mipFilter = MTLSamplerMipFilterNotMipmapped;
					break;
				case GS_MIN_FILTER::Nearest_Mipmap_Nearest:
					sdesc.minFilter = MTLSamplerMinMagFilterNearest;
					sdesc.mipFilter = MTLSamplerMipFilterNearest;
					break;
				case GS_MIN_FILTER::Nearest_Mipmap_Linear:
					sdesc.minFilter = MTLSamplerMinMagFilterNearest;
					sdesc.mipFilter = MTLSamplerMipFilterLinear;
					break;
				case GS_MIN_FILTER::Linear_Mipmap_Nearest:
					sdesc.minFilter = MTLSamplerMinMagFilterLinear;
					sdesc.mipFilter = MTLSamplerMipFilterNearest;
					break;
				case GS_MIN_FILTER::Linear_Mipmap_Linear:
					sdesc.minFilter = MTLSamplerMinMagFilterLinear;
					sdesc.mipFilter = MTLSamplerMipFilterLinear;
					break;
			}

			sdesc.sAddressMode = sel.tau ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
			sdesc.tAddressMode = sel.tav ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
			sdesc.rAddressMode = MTLSamplerAddressModeClampToEdge;

			sdesc.maxAnisotropy = anisotropy && sel.aniso ? anisotropy : 1;

			m_sampler_hw[i] = [m_dev newSamplerStateWithDescriptor:sdesc];
		}

		// Init depth stencil states
		MTLDepthStencilDescriptor* dssdesc = [MTLDepthStencilDescriptor new];
		MTLStencilDescriptor* stencildesc = [MTLStencilDescriptor new];
		stencildesc.stencilCompareFunction = MTLCompareFunctionAlways;
		stencildesc.depthFailureOperation = MTLStencilOperationKeep;
		stencildesc.stencilFailureOperation = MTLStencilOperationKeep;
		stencildesc.depthStencilPassOperation = MTLStencilOperationReplace;
		stencildesc.readMask = 1;
		stencildesc.writeMask = 1;
		dssdesc.frontFaceStencil = stencildesc;
		dssdesc.backFaceStencil = stencildesc;
		m_dss_destination_alpha = [m_dev newDepthStencilStateWithDescriptor:dssdesc];
		stencildesc.stencilCompareFunction = MTLCompareFunctionEqual;
		for (size_t i = 0; i < std::size(m_dss_hw); i++)
		{
			GSHWDrawConfig::DepthStencilSelector sel;
			sel.key = i;
			if (sel.date)
			{
				if (sel.date_one)
					stencildesc.depthStencilPassOperation = MTLStencilOperationZero;
				else
					stencildesc.depthStencilPassOperation = MTLStencilOperationKeep;
				dssdesc.frontFaceStencil = stencildesc;
				dssdesc.backFaceStencil = stencildesc;
			}
			else
			{
				dssdesc.frontFaceStencil = nil;
				dssdesc.backFaceStencil = nil;
			}
			dssdesc.depthWriteEnabled = sel.zwe ? YES : NO;
			static constexpr MTLCompareFunction ztst[] =
			{
				MTLCompareFunctionNever,
				MTLCompareFunctionAlways,
				MTLCompareFunctionGreaterEqual,
				MTLCompareFunctionGreater,
			};
			dssdesc.depthCompareFunction = ztst[sel.ztst];
			m_dss_hw[i] = [m_dev newDepthStencilStateWithDescriptor:dssdesc];
		}

		// Init HW Vertex Shaders
		for (size_t i = 0; i < std::size(m_hw_vs); i++)
		{
			VSSelector sel;
			sel.key = i;
			setFnConstantB(m_fn_constants, sel.fst, GSMTLConstantIndex_FST);
			setFnConstantB(m_fn_constants, sel.iip, GSMTLConstantIndex_IIP);
			m_hw_vs[i] = LoadShader(@"vs_main");
		}

		// Init pipelines
		auto vs_convert = LoadShader(@"vs_convert");
		auto ps_copy = LoadShader(@"ps_copy");
		auto ps_f2i = LoadShader(@"ps_convert_float32_32bits");
		auto pdesc = [MTLRenderPipelineDescriptor new];
		pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		applyAttribute(pdesc.vertexDescriptor, 0, MTLVertexFormatFloat2, offsetof(ConvertShaderVertex, pos),    0);
		applyAttribute(pdesc.vertexDescriptor, 1, MTLVertexFormatFloat2, offsetof(ConvertShaderVertex, texpos), 0);
		pdesc.vertexDescriptor.layouts[0].stride = sizeof(ConvertShaderVertex);

		for (size_t i = 0; i < std::size(m_interlace_pipeline); i++)
		{
			NSString* name = [NSString stringWithFormat:@"ps_interlace%zu", i];
			m_interlace_pipeline[i] = MakePipeline(pdesc, vs_convert, LoadShader(name), name);
		}
		for (size_t i = 0; i < std::size(m_convert_pipeline); i++)
		{
			ShaderConvert conv = static_cast<ShaderConvert>(i);
			GSTexture::Format fmt = targetFormat(conv);
			if (conv == ShaderConvert::OSD || conv == ShaderConvert::DATM_0 || conv == ShaderConvert::DATM_1 || fmt == GSTexture::Format::Invalid)
				continue;
			if (fmt == GSTexture::Format::DepthStencil)
			{
				pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
				pdesc.depthAttachmentPixelFormat = GetPixelFormat(fmt);
			}
			else
			{
				pdesc.colorAttachments[0].pixelFormat = GetPixelFormat(fmt);
				pdesc.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
			}
			NSString* name = [NSString stringWithCString:shaderName(conv) encoding:NSUTF8StringEncoding];
			m_convert_pipeline[i] = MakePipeline(pdesc, vs_convert, LoadShader(name), name);
		}
		pdesc.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
		for (size_t i = 0; i < std::size(m_convert_pipeline_copy); i++)
		{
			GSTexture::Format fmt = static_cast<GSTexture::Format>(i);
			if (fmt != GSTexture::Format::Color && fmt != GSTexture::Format::FloatColor && fmt != GSTexture::Format::Backbuffer)
				continue;
			pdesc.colorAttachments[0].pixelFormat = GetPixelFormat(fmt);
			NSString* name = [NSString stringWithFormat:@"copy_%d", i];
			m_convert_pipeline_copy[i] = MakePipeline(pdesc, vs_convert, ps_copy, name);
		}
		pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatR16Uint;
		m_convert_pipeline_f2i[0] = MakePipeline(pdesc, vs_convert, ps_f2i, @"f2i_u16");
		pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
		m_convert_pipeline_f2i[0] = MakePipeline(pdesc, vs_convert, ps_f2i, @"f2i_u32");

		pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		for (size_t i = 0; i < std::size(m_convert_pipeline_copy_mask); i++)
		{
			MTLColorWriteMask mask = MTLColorWriteMaskNone;
			if (i & 1) mask |= MTLColorWriteMaskRed;
			if (i & 2) mask |= MTLColorWriteMaskGreen;
			if (i & 4) mask |= MTLColorWriteMaskBlue;
			if (i & 8) mask |= MTLColorWriteMaskAlpha;
			NSString* name = [NSString stringWithFormat:@"copy_%s%s%s%s", i & 1 ? "r" : "", i & 2 ? "g" : "", i & 4 ? "b" : "", i & 8 ? "a" : ""];
			pdesc.colorAttachments[0].writeMask = mask;
			m_convert_pipeline_copy_mask[i] = MakePipeline(pdesc, vs_convert, ps_copy, name);
		}

		pdesc.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
		pdesc.colorAttachments[0].blendingEnabled = YES;
		pdesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
		pdesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		pdesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		for (size_t i = 0; i < std::size(m_merge_pipeline); i++)
		{
			NSString* name = [NSString stringWithFormat:@"ps_merge%zu", i];
			m_merge_pipeline[i] = MakePipeline(pdesc, vs_convert, LoadShader(name), name);
		}

		pdesc.colorAttachments[0].blendingEnabled = NO;
		pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
		pdesc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
		m_convert_pipeline[static_cast<int>(ShaderConvert::DATM_0)] = MakePipeline(pdesc, vs_convert, LoadShader(@"ps_datm0"), @"datm0");
		m_convert_pipeline[static_cast<int>(ShaderConvert::DATM_1)] = MakePipeline(pdesc, vs_convert, LoadShader(@"ps_datm1"), @"datm1");

		applyAttribute(pdesc.vertexDescriptor, 0, MTLVertexFormatFloat2,           offsetof(GSVertexPT1, p), 0);
		applyAttribute(pdesc.vertexDescriptor, 1, MTLVertexFormatFloat2,           offsetof(GSVertexPT1, t), 0);
		applyAttribute(pdesc.vertexDescriptor, 2, MTLVertexFormatUChar4Normalized, offsetof(GSVertexPT1, c), 0);
		pdesc.vertexDescriptor.layouts[0].stride = sizeof(GSVertexPT1);
		pdesc.colorAttachments[0].blendingEnabled = YES;
		pdesc.colorAttachments[0].pixelFormat = [m_layer pixelFormat];
		pdesc.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
		m_convert_pipeline[static_cast<int>(ShaderConvert::OSD)] = MakePipeline(pdesc, LoadShader(@"vs_osd"), LoadShader(@"ps_osd"), @"osd");
	}
	catch (std::exception& e)
	{
		Console.Error("Metal: Failed to init: %s", e.what());
		return false;
	}
	return true;
}}

void GSDeviceMTL::GetRealSize(int& w, int& h)
{
	if (![NSThread isMainThread])
	{
		dispatch_sync(dispatch_get_main_queue(), [&]{ GetRealSize(w, h); });
		return;
	}

	const NSSize window_size = [m_view convertRectToBacking:[m_view frame]].size;
	w = static_cast<int>(window_size.width);
	h = static_cast<int>(window_size.height);
}

bool GSDeviceMTL::Reset(int w, int h)
{
	GetRealSize(w, h);
	if (!GSDevice::Reset(w, h))
		return false;

	GSTextureMTL* fake_backbuffer = new GSTextureMTL(this, nil, GSTexture::Type::Backbuffer, GSTexture::Format::Backbuffer);
	m_backbuffer = fake_backbuffer;
	fake_backbuffer->SetSize(GSVector2i(w, h));
	[m_layer setDrawableSize:CGSizeMake(w, h)];
	return true;
}

static constexpr ShaderConvert s_present_shader[5] =
{
	ShaderConvert::COPY,
	ShaderConvert::SCANLINE,
	ShaderConvert::DIAGONAL_FILTER,
	ShaderConvert::TRIANGULAR_FILTER,
	ShaderConvert::COMPLEX_FILTER,
};

void GSDeviceMTL::Present(const GSVector4i& r, int shader)
{ @autoreleasepool {

	EndRenderPass();

#ifndef PCSX2_CORE
	int new_width, new_height;
	if (GSCheckForWindowResize(&new_width, &new_height) && !Reset(new_width, new_height))
		return;
#endif

	{
		id<MTLCommandBuffer> cmdbuf = GetRenderCmdBuf();

		// TODO: Use synchronous fetch if vsync is enabled
		dispatch_semaphore_wait(m_outlive->gpu_work_sema, DISPATCH_TIME_FOREVER);
		[cmdbuf addCompletedHandler:[obj = m_outlive](id<MTLCommandBuffer>){ dispatch_semaphore_signal(obj->gpu_work_sema); }];
		id<CAMetalDrawable> drawable = m_drawable_fetcher.GetIfAvailable();

		if (drawable)
		{
			GSScopedDebugGroupMTL dbg(cmdbuf, @"Present");

			if (m_current)
			{
				GSTextureMTL backbuffer(this, drawable.texture, GSTexture::Type::Backbuffer, GSTexture::Format::Backbuffer);
				backbuffer.RequestColorClear(GSVector4::zero());
				StretchRect(m_current, GSVector4(0, 0, 1, 1), &backbuffer, GSVector4(r), s_present_shader[shader], m_linear_present);
				RenderOsd(&backbuffer);
			}

			[cmdbuf presentDrawable:drawable];
		}
		else
		{
			GSScopedDebugGroupMTL dbg(cmdbuf, @"Present Skipped");
		}
	}

	FlushEncoders();

	if (m_capture_frame)
	{
		static u8 s_capturing = 0;
		if (s_capturing > 4)
		{
			s_capturing = 0;
			[[MTLCaptureManager sharedCaptureManager] stopCapture];
		}
		else if (s_capturing > 0)
		{
			s_capturing++;
		}
		else if (m_capture_frame == m_frame)
		{
			if (@available(macOS 10.15, *))
			{
				MTLCaptureDescriptor* desc = [[MTLCaptureDescriptor alloc] init];
				[desc setCaptureObject:m_dev];
				[desc setDestination:MTLCaptureDestinationGPUTraceDocument];
				[desc setOutputURL:[NSURL fileURLWithPath:@"/tmp/PCSX2Capture.gputrace"]];
				NSError* err = nullptr;
				[[MTLCaptureManager sharedCaptureManager] startCaptureWithDescriptor:desc error:&err];
				if (err)
					Console.Error([[NSString stringWithFormat:@"Metal: Failed to start capture: %@", [err localizedDescription]] UTF8String]);
				else
					s_capturing = 1;
			}
			else
			{
				Console.Error("Metal: Failed to start capture: macOS version too old");
			}
		}
	}
}}

void GSDeviceMTL::Present(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader)
{
	pxAssertRel(0, "Don't call this present");
}
void GSDeviceMTL::Flip()
{
	pxAssertRel(0, "Don't call Flip");
}

void GSDeviceMTL::SetVSync(int vsync)
{
	GSDevice::SetVSync(vsync);
	[m_layer setDisplaySyncEnabled:vsync ? YES : NO];
}

void GSDeviceMTL::ClearRenderTarget(GSTexture* t, const GSVector4& c)
{
	if (!t) return;
	static_cast<GSTextureMTL*>(t)->RequestColorClear(c);
}

void GSDeviceMTL::ClearRenderTarget(GSTexture* t, uint32 c)
{
	GSVector4 color = GSVector4::rgba32(c) * (1.f / 255.f);
	ClearRenderTarget(t, color);
}

void GSDeviceMTL::ClearDepth(GSTexture* t)
{
	if (!t) return;
	static_cast<GSTextureMTL*>(t)->RequestDepthClear(0);
}

void GSDeviceMTL::ClearStencil(GSTexture* t, uint8 c)
{
	if (!t) return;
	static_cast<GSTextureMTL*>(t)->RequestStencilClear(c);
}

bool GSDeviceMTL::DownloadTexture(GSTexture* src, const GSVector4i& rect, GSTexture::GSMap& out_map)
{ @autoreleasepool {
	ASSERT(src);
	EndRenderPass();
	GSTextureMTL* msrc = static_cast<GSTextureMTL*>(src);
	out_map.pitch = msrc->PxToBytes(rect.width());
	size_t size = out_map.pitch * rect.height();
	if ([m_texture_download_buf length] < size)
		m_texture_download_buf = [m_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
	pxAssertRel(m_texture_download_buf, "Failed to allocate download buffer (out of memory?)");

	id<MTLCommandBuffer> cmdbuf = GetRenderCmdBuf();
	[cmdbuf pushDebugGroup:@"DownloadTexture"];
	id<MTLBlitCommandEncoder> encoder = [cmdbuf blitCommandEncoder];
	[encoder copyFromTexture:msrc->GetTexture()
	             sourceSlice:0
	             sourceLevel:0
	            sourceOrigin:MTLOriginMake(rect.x, rect.y, 0)
	              sourceSize:MTLSizeMake(rect.width(), rect.height(), 1)
	                toBuffer:m_texture_download_buf
	       destinationOffset:0
	  destinationBytesPerRow:out_map.pitch
	destinationBytesPerImage:size];
	[encoder endEncoding];
	[cmdbuf popDebugGroup];

	FlushEncoders();
	[cmdbuf waitUntilCompleted];

	out_map.bits = static_cast<u8*>([m_texture_download_buf contents]);
	return true;
}}

void GSDeviceMTL::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r)
{ @autoreleasepool {

	GSTextureMTL* sT = static_cast<GSTextureMTL*>(sTex);
	GSTextureMTL* dT = static_cast<GSTextureMTL*>(dTex);

	// Process clears
	GSVector2i dsize = dTex->GetSize();
	if (r.width() < dsize.x || r.height() < dsize.y)
		dT->FlushClears();
	else
		dT->InvalidateClears();

	EndRenderPass();

	sT->m_last_read  = m_current_draw;
	dT->m_last_write = m_current_draw;

	id<MTLCommandBuffer> cmdbuf = GetRenderCmdBuf();
	id<MTLBlitCommandEncoder> encoder = [cmdbuf blitCommandEncoder];
	[encoder setLabel:@"CopyRect"];
	[encoder copyFromTexture:sT->GetTexture()
	             sourceSlice:0
	             sourceLevel:0
	            sourceOrigin:MTLOriginMake(r.x, r.y, 0)
	              sourceSize:MTLSizeMake(r.width(), r.height(), 1)
	               toTexture:dT->GetTexture()
	        destinationSlice:0
	        destinationLevel:0
	       destinationOrigin:MTLOriginMake(0, 0, 0)];
	[encoder endEncoding];
}}

void GSDeviceMTL::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, id<MTLRenderPipelineState> pipeline, bool linear, LoadAction load_action, void* frag_uniform, size_t frag_uniform_len)
{
	BeginScene();

	FlushClears(sTex);

	GSTextureMTL* sT = static_cast<GSTextureMTL*>(sTex);
	GSTextureMTL* dT = static_cast<GSTextureMTL*>(dTex);

	GSVector2i ds = dT->GetSize();

	bool covers_target = static_cast<int>(dRect.x) <= 0
	                  && static_cast<int>(dRect.y) <= 0
	                  && static_cast<int>(dRect.z) >= ds.x
	                  && static_cast<int>(dRect.w) >= ds.y;
	bool dontcare = load_action == LoadAction::DontCare || (load_action == LoadAction::DontCareIfFull && covers_target);
	MTLLoadAction action = dontcare ? MTLLoadActionDontCare : MTLLoadActionLoad;

	MainRenderEncoder* enc;
	if (dT->GetFormat() == GSTexture::Format::DepthStencil)
		enc = &BeginRenderPass(nullptr, MTLLoadActionDontCare, dT, action);
	else
		enc = &BeginRenderPass(dT, action, nullptr, MTLLoadActionDontCare);

	enc->ClearScissor();
	DepthStencilSelector dsel;
	dsel.ztst = ZTST_ALWAYS;
	dsel.zwe = dT->GetFormat() == GSTexture::Format::DepthStencil;
	SetDSS(*enc, dsel);

	float left = dRect.x * 2 / ds.x - 1.0f;
	float right = dRect.z * 2 / ds.x - 1.0f;
	float top = 1.0f - dRect.y * 2 / ds.y;
	float bottom = 1.0f - dRect.w * 2 / ds.y;

	ConvertShaderVertex vertices[] =
	{
		{{left,  top},    {sRect.x, sRect.y}},
		{{right, top},    {sRect.z, sRect.y}},
		{{left,  bottom}, {sRect.x, sRect.w}},
		{{right, bottom}, {sRect.z, sRect.w}}
	};

	[enc->encoder setLabel:@"StretchRect"];
	enc->SetPipeline(pipeline);
	enc->SetVertexBytes(vertices, sizeof(vertices));
	SetTexture(*enc, sT, 0);

	if (frag_uniform && frag_uniform_len)
		enc->SetPSCB(frag_uniform, frag_uniform_len);

	SetSampler(*enc, linear ? SamplerSelector::Linear() : SamplerSelector::Point());

	[enc->encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
	                 vertexStart:0
	                 vertexCount:4];

	EndScene();
}

void GSDeviceMTL::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{ @autoreleasepool {
	id<MTLRenderPipelineState> pipeline;
	if (shader == ShaderConvert::COPY)
		pipeline = m_convert_pipeline_copy[static_cast<int>(dTex->GetFormat())];
	else if (shader == ShaderConvert::FLOAT32_TO_32_BITS)
		pipeline = m_convert_pipeline_f2i[dTex->GetFormat() == GSTexture::Format::UInt16 ? 0 : 1];
	else
		pipeline = m_convert_pipeline[static_cast<int>(shader)];

	bool is_opaque = shader != ShaderConvert::DATM_0 && shader != ShaderConvert::DATM_1;

	DoStretchRect(sTex, sRect, dTex, dRect, pipeline, linear, is_opaque ? LoadAction::DontCareIfFull : LoadAction::Load, nullptr, 0);
}}

void GSDeviceMTL::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha)
{ @autoreleasepool {
	int sel = 0;
	if (red)   sel |= 1;
	if (green) sel |= 2;
	if (blue)  sel |= 4;
	if (alpha) sel |= 8;

	id<MTLRenderPipelineState> pipeline = m_convert_pipeline_copy_mask[sel];

	DoStretchRect(sTex, sRect, dTex, dRect, pipeline, false, sel == 15 ? LoadAction::DontCareIfFull : LoadAction::Load, nullptr, 0);
}}

void GSDeviceMTL::FlushClears(GSTexture* tex)
{
	if (tex)
		static_cast<GSTextureMTL*>(tex)->FlushClears();
}

// MARK: - MainRenderEncoder setters

void GSDeviceMTL::SetHWPipelineState(MainRenderEncoder& enc, GSHWDrawConfig::VSSelector vssel, GSHWDrawConfig::PSSelector pssel, PipelineSelectorExtrasMTL extras)
{
	PipelineSelectorMTL fullsel(vssel, pssel, extras);
	if (enc.has_pipeline_sel && fullsel == enc.pipeline_sel)
		return;
	auto idx = m_hw_pipeline.find(fullsel);
	if (idx != m_hw_pipeline.end())
	{
		[enc.encoder setRenderPipelineState:idx->second];
		enc.pipeline_sel = fullsel;
		enc.has_pipeline_sel = true;
		return;
	}

	VSSelector vssel_mtl;
	vssel_mtl.fst = vssel.fst;
	vssel_mtl.iip = pssel.iip;
	id<MTLFunction> vs = m_hw_vs[vssel_mtl.key];

	id<MTLFunction> ps;
	auto idx2 = m_hw_ps.find(pssel.key);
	if (idx2 != m_hw_ps.end())
	{
		ps = idx2->second;
	}
	else
	{
		setFnConstantB(m_fn_constants, pssel.fst,                GSMTLConstantIndex_FST);
		setFnConstantB(m_fn_constants, pssel.iip,                GSMTLConstantIndex_IIP);
		setFnConstantI(m_fn_constants, pssel.aem_fmt,            GSMTLConstantIndex_PS_AEM_FMT);
		setFnConstantI(m_fn_constants, pssel.pal_fmt,            GSMTLConstantIndex_PS_PAL_FMT);
		setFnConstantI(m_fn_constants, pssel.dfmt,               GSMTLConstantIndex_PS_DFMT);
		setFnConstantI(m_fn_constants, pssel.depth_fmt,          GSMTLConstantIndex_PS_DEPTH_FMT);
		setFnConstantB(m_fn_constants, pssel.aem,                GSMTLConstantIndex_PS_AEM);
		setFnConstantB(m_fn_constants, pssel.fba,                GSMTLConstantIndex_PS_FBA);
		setFnConstantB(m_fn_constants, pssel.fog,                GSMTLConstantIndex_PS_FOG);
		setFnConstantI(m_fn_constants, pssel.date,               GSMTLConstantIndex_PS_DATE);
		setFnConstantI(m_fn_constants, pssel.atst,               GSMTLConstantIndex_PS_ATST);
		setFnConstantB(m_fn_constants, pssel.fst,                GSMTLConstantIndex_PS_FST);
		setFnConstantI(m_fn_constants, pssel.tfx,                GSMTLConstantIndex_PS_TFX);
		setFnConstantB(m_fn_constants, pssel.tcc,                GSMTLConstantIndex_PS_TCC);
		setFnConstantI(m_fn_constants, pssel.wms,                GSMTLConstantIndex_PS_WMS);
		setFnConstantI(m_fn_constants, pssel.wmt,                GSMTLConstantIndex_PS_WMT);
		setFnConstantB(m_fn_constants, pssel.ltf,                GSMTLConstantIndex_PS_LTF);
		setFnConstantB(m_fn_constants, pssel.shuffle,            GSMTLConstantIndex_PS_SHUFFLE);
		setFnConstantB(m_fn_constants, pssel.read_ba,            GSMTLConstantIndex_PS_READ_BA);
		setFnConstantB(m_fn_constants, pssel.write_rg,           GSMTLConstantIndex_PS_WRITE_RG);
		setFnConstantB(m_fn_constants, pssel.fbmask,             GSMTLConstantIndex_PS_FBMASK);
		setFnConstantI(m_fn_constants, pssel.blend_a,            GSMTLConstantIndex_PS_BLEND_A);
		setFnConstantI(m_fn_constants, pssel.blend_b,            GSMTLConstantIndex_PS_BLEND_B);
		setFnConstantI(m_fn_constants, pssel.blend_c,            GSMTLConstantIndex_PS_BLEND_C);
		setFnConstantI(m_fn_constants, pssel.blend_d,            GSMTLConstantIndex_PS_BLEND_D);
		setFnConstantB(m_fn_constants, pssel.clr1,               GSMTLConstantIndex_PS_CLR1);
		setFnConstantB(m_fn_constants, pssel.hdr,                GSMTLConstantIndex_PS_HDR);
		setFnConstantB(m_fn_constants, pssel.colclip,            GSMTLConstantIndex_PS_COLCLIP);
		setFnConstantB(m_fn_constants, pssel.pabe,               GSMTLConstantIndex_PS_PABE);
		setFnConstantI(m_fn_constants, pssel.channel,            GSMTLConstantIndex_PS_CHANNEL);
		setFnConstantI(m_fn_constants, pssel.dither,             GSMTLConstantIndex_PS_DITHER);
		setFnConstantB(m_fn_constants, pssel.zclamp,             GSMTLConstantIndex_PS_ZCLAMP);
		setFnConstantB(m_fn_constants, pssel.tcoffsethack,       GSMTLConstantIndex_PS_TCOFFSETHACK);
		setFnConstantB(m_fn_constants, pssel.urban_chaos_hle,    GSMTLConstantIndex_PS_URBAN_CHAOS_HLE);
		setFnConstantB(m_fn_constants, pssel.tales_of_abyss_hle, GSMTLConstantIndex_PS_TALES_OF_ABYSS_HLE);
		setFnConstantB(m_fn_constants, pssel.tex_is_fb,          GSMTLConstantIndex_PS_TEX_IS_FB);
		setFnConstantB(m_fn_constants, pssel.automatic_lod,      GSMTLConstantIndex_PS_AUTOMATIC_LOD);
		setFnConstantB(m_fn_constants, pssel.manual_lod,         GSMTLConstantIndex_PS_MANUAL_LOD);
		setFnConstantB(m_fn_constants, pssel.point_sampler,      GSMTLConstantIndex_PS_POINT_SAMPLER);
		setFnConstantB(m_fn_constants, pssel.invalid_tex0,       GSMTLConstantIndex_PS_INVALID_TEX0);
		bool early_fragment = pssel.date == 1 || pssel.date == 2;
		ps = LoadShader(early_fragment ? @"ps_main_eft" : @"ps_main");
		m_hw_ps.insert(std::make_pair(pssel.key, ps));
	}

	MTLRenderPipelineDescriptor* pdesc = [MTLRenderPipelineDescriptor new];
	pdesc.vertexDescriptor = m_hw_vertex;
	MTLRenderPipelineColorAttachmentDescriptor* color = pdesc.colorAttachments[0];
	color.pixelFormat = GetPixelFormat(extras.rt);
	pdesc.depthAttachmentPixelFormat = extras.has_depth ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid;
	pdesc.stencilAttachmentPixelFormat = extras.has_stencil ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid;
	color.writeMask = extras.writemask;
	if (extras.blend)
	{
		HWBlend b = GetBlend(extras.blend);
		if (extras.accumulation_blend)
			b.src = b.dst = MTLBlendFactorOne;
		if (extras.mixed_hw_sw_blend)
			b.src = MTLBlendFactorOne;
		color.blendingEnabled = YES;
		color.rgbBlendOperation = static_cast<MTLBlendOperation>(b.op);
		color.sourceRGBBlendFactor = static_cast<MTLBlendFactor>(b.src);
		color.destinationRGBBlendFactor = static_cast<MTLBlendFactor>(b.dst);
	}
	id<MTLRenderPipelineState> pipeline = MakePipeline(pdesc, vs, ps, [NSString stringWithFormat:@"HW Render %llx", pssel.key]);
	m_hw_pipeline.insert(std::make_pair(fullsel, pipeline));

	[enc.encoder setRenderPipelineState:pipeline];
}

void GSDeviceMTL::SetDSS(MainRenderEncoder& enc, DepthStencilSelector sel)
{
	if (enc.has_depth_sel && enc.depth_sel.key == sel.key)
		return;
	[enc.encoder setDepthStencilState:m_dss_hw[sel.key]];
	enc.depth_sel = sel;
	enc.has_depth_sel = true;
}

void GSDeviceMTL::SetSampler(MainRenderEncoder& enc, SamplerSelector sel)
{
	if (enc.has_sampler && enc.sampler_sel.key == sel.key)
		return;
	[enc.encoder setFragmentSamplerState:m_sampler_hw[sel.key] atIndex:0];
	enc.sampler_sel = sel;
	enc.has_sampler = true;
}

static void textureBarrier(id<MTLRenderCommandEncoder> enc)
{
	if (@available(macOS 10.14, *)) {
		[enc memoryBarrierWithScope:MTLBarrierScopeRenderTargets
		                afterStages:MTLRenderStageVertex
		               beforeStages:MTLRenderStageFragment];
	} else {
		[enc textureBarrier];
	}
}

void GSDeviceMTL::SetTexture(MainRenderEncoder& enc, GSTexture* tex, int pos)
{
	if (tex == enc.tex[pos])
		return;
	enc.tex[pos] = tex;
	if (GSTextureMTL* mtex = static_cast<GSTextureMTL*>(tex))
	{
		[enc.encoder setFragmentTexture:mtex->GetTexture() atIndex:pos];
		mtex->m_last_read = m_current_draw;
	}
}

void GSDeviceMTL::MainRenderEncoder::SetVertices(id<MTLBuffer> buffer, size_t offset)
{
	if (vertex_buffer != (__bridge void*)buffer)
	{
		vertex_buffer = (__bridge void*)buffer;
		[encoder setVertexBuffer:buffer offset:offset atIndex:GSMTLBufferIndexVertices];
	}
	else
	{
		[encoder setVertexBufferOffset:offset atIndex:GSMTLBufferIndexVertices];
	}
}

void GSDeviceMTL::MainRenderEncoder::SetVertexBytes(void* buffer, size_t size)
{
	vertex_buffer = nullptr;
	[encoder setVertexBytes:buffer length:size atIndex:GSMTLBufferIndexVertices];
}

void GSDeviceMTL::MainRenderEncoder::SetScissor(const GSVector4i& scissor)
{
	if (has_scissor && (this->scissor == scissor).alltrue())
		return;
	MTLScissorRect r;
	r.x = scissor.x;
	r.y = scissor.y;
	r.width = scissor.width();
	r.height = scissor.height();
	[encoder setScissorRect:r];
	this->scissor = scissor;
	has_scissor = true;
}

void GSDeviceMTL::MainRenderEncoder::ClearScissor()
{
	if (!has_scissor)
		return;
	has_scissor = false;
	GSVector4i size = GSVector4i(0);
	if (color_target)   size = size.max_u32(GSVector4i(color_target  ->GetSize()));
	if (depth_target)   size = size.max_u32(GSVector4i(depth_target  ->GetSize()));
	if (stencil_target) size = size.max_u32(GSVector4i(stencil_target->GetSize()));
	MTLScissorRect r;
	r.x = 0;
	r.y = 0;
	r.width = size.x;
	r.height = size.y;
	[encoder setScissorRect:r];
}

template <typename Dst, typename Src>
void bitcpy(Dst& dst, const Src& src)
{
	static_assert(sizeof(dst) == sizeof(src), "Must be the same size");
	memcpy(&dst, &src, sizeof(dst));
}

static GSMTLMainVSUniform convertCB(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	GSMTLMainVSUniform ret;
	memset(&ret, 0, sizeof(ret));
	ret.vertex_scale.x = cb.vertex_scale.x;
	ret.vertex_scale.y = -cb.vertex_scale.y;
	ret.vertex_offset.x = cb.vertex_offset.x;
	ret.vertex_offset.y = -cb.vertex_offset.y;
	bitcpy(ret.texture_offset, cb.texture_offset);
	bitcpy(ret.texture_scale, cb.texture_scale);
	bitcpy(ret.max_depth, cb.max_depth.x);
	return ret;
}

static GSMTLMainPSUniform convertCB(const GSHWDrawConfig::PSConstantBuffer& cb, int atst)
{
	GSMTLMainPSUniform ret;
	memset(&ret, 0, sizeof(ret));
	bitcpy(ret.fog_color_aref, GSVector4(GSVector4i::load(cb.fog_color_aref).u8to32()));
	if (atst == 1 || atst == 2) // Greater / Less alpha
		ret.fog_color_aref.a -= 0.1;
	bitcpy(ret.wh, cb.texture_size);
	ret.ta.x = static_cast<float>(cb.ta0) / 255.f;
	ret.ta.y = static_cast<float>(cb.ta1) / 255.f;
	ret.alpha_fix = static_cast<float>(cb.alpha_fix) / 128.f;
	bitcpy(ret.uv_msk_fix, cb.uv_msk_fix);
	bitcpy(ret.fbmask, cb.fbmask_int);
	bitcpy(ret.half_texel, cb.half_texel);
	bitcpy(ret.uv_min_max, cb.uv_min_max);
	bitcpy(ret.tc_offset, cb.tc_offset);
	ret.max_depth = cb.max_depth * 0x1p-32f;
	bitcpy(ret.dither_matrix, cb.dither_matrix);
	ret.channel_shuffle_int = cb.channel_shuffle_int;
	return ret;
}


void GSDeviceMTL::MainRenderEncoder::SetCB(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (has_cb_vs && cb_vs == cb)
		return;
	GSMTLMainVSUniform cb_mtl = convertCB(cb);
	[encoder setVertexBytes:&cb_mtl length:sizeof(cb_mtl) atIndex:GSMTLBufferIndexUniforms];
	has_cb_vs = true;
	memcpy(&cb_vs, &cb, sizeof(cb));
}

void GSDeviceMTL::MainRenderEncoder::SetCB(const GSHWDrawConfig::PSConstantBuffer& cb, int atst)
{
	if (has_cb_ps && cb_ps == cb && (atst == 1 || atst == 2) == cb_ps_aref_off)
		return;
	GSMTLMainPSUniform cb_mtl = convertCB(cb, atst);
	[encoder setFragmentBytes:&cb_mtl length:sizeof(cb_mtl) atIndex:GSMTLBufferIndexUniforms];
	has_cb_ps = true;
	cb_ps_aref_off = (atst == 1 || atst == 2);
	memcpy(&cb_ps, &cb, sizeof(cb));
}

void GSDeviceMTL::MainRenderEncoder::SetPSCB(const void* bytes, size_t len)
{
	has_cb_ps = false;
	[encoder setFragmentBytes:bytes length:len atIndex:GSMTLBufferIndexUniforms];
}

void GSDeviceMTL::MainRenderEncoder::SetBlendColor(u8 color)
{
	if (has_blend_color && blend_color == color)
		return;
	float fc = static_cast<float>(color) / 128.f;
	[encoder setBlendColorRed:fc green:fc blue:fc alpha:fc];
	has_blend_color = true;
	blend_color = color;
}

void GSDeviceMTL::MainRenderEncoder::SetPipeline(id<MTLRenderPipelineState> pipe)
{
	if (!has_pipeline_sel && pipeline == (__bridge void*)pipe)
		return;
	pipeline = (__bridge void*)pipe;
	[encoder setRenderPipelineState:pipe];
	has_pipeline_sel = false;
}

void GSDeviceMTL::MainRenderEncoder::SetDepth(id<MTLDepthStencilState> dss)
{
	if (!has_depth_sel && depth == (__bridge void*)dss)
		return;
	depth = (__bridge void*)dss;
	[encoder setDepthStencilState:dss];
	has_depth_sel = false;
}

// MARK: - HW Render

void GSDeviceMTL::SetupDestinationAlpha(GSTexture* rt, GSTexture* ds, const GSVector4i& r, bool datm)
{
	BeginScene();
	FlushClears(rt);
	GSTextureMTL* mds = static_cast<GSTextureMTL*>(ds);
	mds->RequestStencilClear(0);
	auto& enc = BeginRenderPass(nullptr, MTLLoadActionDontCare, nullptr, MTLLoadActionDontCare, ds, MTLLoadActionLoad);
	[enc.encoder setLabel:@"Destination Alpha Setup"];
	enc.SetDepth(m_dss_destination_alpha);
	[enc.encoder setStencilReferenceValue:1];
	enc.SetPipeline(m_convert_pipeline[static_cast<int>(datm ? ShaderConvert::DATM_1 : ShaderConvert::DATM_0)]);
	enc.SetScissor(r);
	const GSVector4 src = GSVector4(r) / GSVector4(ds->GetSize()).xyxy();
	const GSVector4 dst = src * 2.f - 1.f;
	ConvertShaderVertex vertices[] =
	{
		{{dst.x, -dst.y}, {src.x, src.y}},
		{{dst.z, -dst.y}, {src.z, src.y}},
		{{dst.x, -dst.w}, {src.x, src.w}},
		{{dst.z, -dst.w}, {src.z, src.w}},
	};
	SetTexture(enc, rt, 0);
	SetSampler(enc, SamplerSelector::Point());
	enc.SetVertexBytes(vertices, sizeof(vertices));
	[enc.encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	EndScene();
}

static id<MTLTexture> getTexture(GSTexture* tex)
{
	return tex ? static_cast<GSTextureMTL*>(tex)->GetTexture() : nil;
}

void GSDeviceMTL::RenderHW(GSHWDrawConfig& config)
{ @autoreleasepool {

	GSTexture* stencil = nullptr;
	GSTexture* rt = config.rt;
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off:
		case GSHWDrawConfig::DestinationAlphaMode::Full:
			break; // No setup
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking:
			ASSERT(0 && "Unsupported");
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne:
			ClearStencil(config.ds, 1);
			stencil = config.ds;
			break;
		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
		{
			SetupDestinationAlpha(config.rt, config.ds, config.scissor, config.datm);
			stencil = config.ds;
		}
	}

	BeginScene();
	GSTexture* hdr_rt = nullptr;
	if (config.ps.hdr)
	{
		GSVector2i size = config.rt->GetSize();
		hdr_rt = CreateRenderTarget(size.x, size.y, GSTexture::Format::FloatColor);
		GSVector4 srect = GSVector4(config.scissor) / GSVector4(size).xyxy();
		DoStretchRect(config.rt, srect, hdr_rt, GSVector4(config.scissor), m_convert_pipeline_copy[static_cast<int>(GSTexture::Format::FloatColor)], false, LoadAction::DontCare, nullptr, 0);
		rt = hdr_rt;
	}

	FlushClears(config.tex);
	FlushClears(config.pal);
	FlushClears(config.raw_tex);

	auto& enc = BeginRenderPass(rt, MTLLoadActionLoad, config.ds, MTLLoadActionLoad, stencil, MTLLoadActionLoad);
	id<MTLRenderCommandEncoder> mtlenc = enc.encoder;
	[mtlenc setLabel:@"RenderHW"];
	enc.SetScissor(config.scissor);
	SetTexture(enc, config.tex,     GSMTLTextureIndexTex);
	SetTexture(enc, config.pal,     GSMTLTextureIndexPalette);
	SetTexture(enc, config.raw_tex, GSMTLTextureIndexRawTex);
	SetTexture(enc, config.rt,      GSMTLTextureIndexRenderTarget);
	SetSampler(enc, config.sampler);
	if (config.blend.index && config.blend.is_constant)
		enc.SetBlendColor(config.blend.factor);
	PipelineSelectorExtrasMTL sel(config.blend, rt, config.colormask, config.ds, stencil);
	SetHWPipelineState(enc, config.vs, config.ps, sel);
	SetDSS(enc, config.depth);

	enc.SetCB(config.cb_vs);
	enc.SetCB(config.cb_ps, config.ps.atst);

	size_t vertsize = config.nverts * sizeof(*config.verts);
	size_t idxsize = config.nindices * sizeof(*config.indices);
	Map allocation = Allocate(m_vertex_upload_buf, vertsize + idxsize);

	enc.SetVertices(allocation.gpu_buffer, allocation.gpu_offset);
	memcpy(allocation.cpu_buffer, config.verts, vertsize);
	memcpy(static_cast<u8*>(allocation.cpu_buffer) + vertsize, config.indices, idxsize);

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		// TODO: Implement
	}

	SendHWDraw(config, mtlenc, allocation.gpu_buffer, allocation.gpu_offset + vertsize);

	if (config.alpha_second_pass.enable)
	{
		enc.SetCB(config.alpha_second_pass.cb_ps, config.alpha_second_pass.ps.atst);
		sel = PipelineSelectorExtrasMTL(config.blend, rt, config.alpha_second_pass.colormask, config.ds, stencil);
		SetHWPipelineState(enc, config.vs, config.ps, sel);
		SetDSS(enc, config.alpha_second_pass.depth);
		SendHWDraw(config, mtlenc, allocation.gpu_buffer, allocation.gpu_offset + vertsize);
	}

	if (hdr_rt)
	{
		GSVector2i size = config.rt->GetSize();
		GSVector4 dRect(config.scissor);
		const GSVector4 sRect = dRect / GSVector4(size).xyxy();
		StretchRect(hdr_rt, sRect, config.rt, dRect, ShaderConvert::MOD_256, false);

		Recycle(hdr_rt);
	}
}}

void GSDeviceMTL::SendHWDraw(GSHWDrawConfig& config, id<MTLRenderCommandEncoder> enc, id<MTLBuffer> buffer, size_t off)
{
	MTLPrimitiveType topology;
	switch (config.topology)
	{
		case GSHWDrawConfig::Topology::Point:    topology = MTLPrimitiveTypePoint;    break;
		case GSHWDrawConfig::Topology::Line:     topology = MTLPrimitiveTypeLine;     break;
		case GSHWDrawConfig::Topology::Triangle: topology = MTLPrimitiveTypeTriangle; break;
	}

	if (config.drawlist)
	{
		[enc pushDebugGroup:[NSString stringWithFormat:@"Full barrier split draw (%d sprites in %d groups)", config.nindices / config.indices_per_prim, config.drawlist->size()]];
#if defined(_DEBUG)
		// Check how draw call is split.
		std::map<size_t, size_t> frequency;
		for (const auto& it : *config.drawlist)
			++frequency[it];

		std::string message;
		for (const auto& it : frequency)
			message += " " + std::to_string(it.first) + "(" + std::to_string(it.second) + ")";

		[enc insertDebugSignpost:[NSString stringWithFormat:@"Split single draw (%d sprites) into %zu draws: consecutive draws(frequency):%s",
			config.nindices / config.indices_per_prim, config.drawlist->size(), message.c_str()]];
#endif

		for (size_t count = 0, p = 0, n = 0; n < config.drawlist->size(); p += count, ++n)
		{
			count = (*config.drawlist)[n] * config.indices_per_prim;
			textureBarrier(enc);
			[enc drawIndexedPrimitives:topology
			                indexCount:count
			                 indexType:MTLIndexTypeUInt32
			               indexBuffer:buffer
			         indexBufferOffset:off + p * sizeof(*config.indices)];
		}
		[enc popDebugGroup];
	}
	else if (config.require_full_barrier)
	{
		[enc pushDebugGroup:[NSString stringWithFormat:@"Full barrier split draw (%d prims)", config.nindices / config.indices_per_prim]];
		for (size_t p = 0; p < config.nindices; p += config.indices_per_prim)
		{
			textureBarrier(enc);
			[enc drawIndexedPrimitives:topology
			                indexCount:config.indices_per_prim
			                 indexType:MTLIndexTypeUInt32
			               indexBuffer:buffer
			         indexBufferOffset:off + p * sizeof(*config.indices)];
		}
		[enc popDebugGroup];
	}
	else if (config.require_one_barrier)
	{
		// One barrier needed
		textureBarrier(enc);
		[enc drawIndexedPrimitives:topology
		                indexCount:config.nindices
		                 indexType:MTLIndexTypeUInt32
		               indexBuffer:buffer
		         indexBufferOffset:off];
	}
	else
	{
		// No barriers needed
		[enc drawIndexedPrimitives:topology
		                indexCount:config.nindices
		                 indexType:MTLIndexTypeUInt32
		               indexBuffer:buffer
		         indexBufferOffset:off];
	}
}

// MARK: - OSD

void GSDeviceMTL::RenderOsd(GSTexture* dt)
{
	size_t count = m_osd.Size();
	if (!count)
		return;
	BeginScene();
	auto& enc = BeginRenderPass(dt, MTLLoadActionLoad, nullptr, MTLLoadActionDontCare);
	id<MTLRenderCommandEncoder> mtlenc = enc.encoder;
	[mtlenc pushDebugGroup:@"RenderOSD"];

	if (m_osd.m_texture_dirty)
		m_osd.upload_texture_atlas(m_font.get());

	SetDSS(enc, DepthStencilSelector::NoDepth());
	SetTexture(enc, m_font.get(), 0);
	enc.ClearScissor();
	enc.SetPipeline(m_convert_pipeline[static_cast<int>(ShaderConvert::OSD)]);
	Map map = Allocate(m_vertex_upload_buf, count * sizeof(GSVertexPT1));
	count = m_osd.GeneratePrimitives(static_cast<GSVertexPT1*>(map.cpu_buffer), count);
	enc.SetVertices(map.gpu_buffer, map.gpu_offset);
	[mtlenc drawPrimitives:MTLPrimitiveTypeTriangle
	           vertexStart:0
	           vertexCount:count];

	[mtlenc popDebugGroup];
	EndScene();
}

#endif // __APPLE__
