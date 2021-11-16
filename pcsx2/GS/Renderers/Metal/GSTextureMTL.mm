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
#include "GSTextureMTL.h"
#include "GSDeviceMTL.h"

#if ! __has_feature(objc_arc)
	#error "Compile this with -fobjc-arc"
#endif

#ifdef __APPLE__

GSTextureMTL::GSTextureMTL(GSDeviceMTL* dev, id<MTLTexture> texture, Type type, Format format)
	: m_dev(dev)
	, m_texture(texture)
{
	m_type = type;
	m_format = format;
	m_size.x = m_texture.width;
	m_size.y = m_texture.height;
	m_max_layer = m_texture.mipmapLevelCount;

	switch (format)
	{
		case Format::UNorm8:
			m_int_shift = 0;
			break;
		case Format::UInt16:
			m_int_shift = 1;
			break;
		case Format::UInt32:
		case Format::Int32:
		case Format::Backbuffer:
		case Format::Color:
			m_int_shift = 2;
			break;
		case Format::DepthStencil:
			m_int_shift = 3;
			break;
		case Format::FloatColor:
			m_int_shift = 4;
			break;
		case Format::Invalid:
			m_int_shift = 0;
			ASSERT(0);
	}
}
GSTextureMTL::~GSTextureMTL()
{
}

void GSTextureMTL::RequestColorClear(GSVector4 color)
{
	m_needs_color_clear = true;
	m_clear_color = color;
}
void GSTextureMTL::RequestDepthClear(float depth)
{
	m_needs_depth_clear = true;
	m_clear_depth = depth;
}
void GSTextureMTL::RequestStencilClear(int stencil)
{
	m_needs_stencil_clear;
	m_clear_stencil = stencil;
}
bool GSTextureMTL::GetResetNeedsColorClear(GSVector4& colorOut)
{
	if (m_needs_color_clear)
	{
		m_needs_color_clear = false;
		colorOut = m_clear_color;
		return true;
	}
	return false;
}
bool GSTextureMTL::GetResetNeedsDepthClear(float& depthOut)
{
	if (m_needs_depth_clear)
	{
		m_needs_depth_clear = false;
		depthOut = m_clear_depth;
		return true;
	}
	return false;
}
bool GSTextureMTL::GetResetNeedsStencilClear(int& stencilOut)
{
	if (m_needs_stencil_clear)
	{
		m_needs_stencil_clear = false;
		stencilOut = m_clear_stencil;
		return true;
	}
	return false;
}

void GSTextureMTL::ApplyColorLoadAction(MTLRenderPassDescriptor* desc, MTLLoadAction base)
{
	if (m_needs_color_clear)
	{
		m_needs_color_clear = false;
		if (base == MTLLoadActionDontCare)
		{
			desc.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		}
		else
		{
			desc.colorAttachments[0].clearColor = MTLClearColorMake(m_clear_color.r, m_clear_color.g, m_clear_color.b, m_clear_color.a);
			desc.colorAttachments[0].loadAction = MTLLoadActionClear;
		}
	}
	else
	{
		desc.colorAttachments[0].loadAction = base;
	}
}

void GSTextureMTL::ApplyDepthLoadAction(MTLRenderPassDescriptor* desc, MTLLoadAction base)
{
	if (m_needs_depth_clear)
	{
		m_needs_depth_clear = false;
		if (base == MTLLoadActionDontCare)
		{
			desc.depthAttachment.loadAction = MTLLoadActionDontCare;
		}
		else
		{
			desc.depthAttachment.clearDepth = m_clear_depth;
			desc.depthAttachment.loadAction = MTLLoadActionClear;
		}
	}
	else
	{
		desc.depthAttachment.loadAction = base;
	}
}

bool GSTextureMTL::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{ @autoreleasepool {
	if (layer >= m_max_layer)
		return false;
	m_has_mipmaps = false;

	if (m_last_read == m_dev->m_current_draw)
	{
		[m_dev->m_current_draw_encoder insertDebugSignpost:@"Early flush due to upload to already-used texture"];
		m_dev->FlushEncoders();
	}

	int size = pitch * r.height();
	GSDeviceMTL::Map map = m_dev->Allocate(m_dev->m_texture_upload_buf, size);
	memcpy(map.cpu_buffer, data, size);

	id<MTLBlitCommandEncoder> enc = m_dev->GetTextureUploadEncoder();
	[enc copyFromBuffer:map.gpu_buffer
	       sourceOffset:map.gpu_offset
	  sourceBytesPerRow:pitch
	sourceBytesPerImage:size
	         sourceSize:MTLSizeMake(r.width(), r.height(), 1)
	          toTexture:m_texture
	   destinationSlice:0
	   destinationLevel:layer
	  destinationOrigin:MTLOriginMake(r.x, r.y, 0)];
	return true;
}}

bool GSTextureMTL::Map(GSMap& m, const GSVector4i* _r, int layer)
{ @autoreleasepool {
	if (layer >= m_max_layer)
		return false;
	m_has_mipmaps = false;

	if (m_last_read == m_dev->m_current_draw)
	{
		[m_dev->m_current_draw_encoder insertDebugSignpost:@"Early flush due to upload to already-used texture"];
		m_dev->FlushEncoders();
	}

	GSVector4i r = _r ? *_r : GSVector4i(0, 0, m_size.x, m_size.y);
	m.pitch = r.width() << m_int_shift;
	size_t size = m.pitch * r.height();
	GSDeviceMTL::Map map = m_dev->Allocate(m_dev->m_texture_upload_buf, size);

	id<MTLBlitCommandEncoder> enc = m_dev->GetTextureUploadEncoder();
	// Copy is scheduled now, won't happen until the encoder is committed so no problems with ordering
	[enc copyFromBuffer:map.gpu_buffer
	       sourceOffset:map.gpu_offset
	  sourceBytesPerRow:m.pitch
	sourceBytesPerImage:size
	         sourceSize:MTLSizeMake(r.width(), r.height(), 1)
	          toTexture:m_texture
	   destinationSlice:0
	   destinationLevel:layer
	  destinationOrigin:MTLOriginMake(r.x, r.y, 0)];

	m.bits = static_cast<u8*>(map.cpu_buffer);
	return true;
}}

void GSTextureMTL::Unmap()
{
	// Nothing to do here, upload is already scheduled
}

void GSTextureMTL::GenerateMipmap()
{ @autoreleasepool {
	if (m_max_layer > 1 && !m_has_mipmaps)
	{
		id<MTLBlitCommandEncoder> enc = m_dev->GetTextureUploadEncoder();
		[enc generateMipmapsForTexture:m_texture];
	}
}}

bool GSTextureMTL::Save(const std::string& fn)
{
	// TODO: Implement
	return false;
}

#endif
