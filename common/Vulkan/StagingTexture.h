/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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
#include "common/Vulkan/StagingBuffer.h"
#include "common/Vulkan/Texture.h"

namespace Vulkan
{
	class StagingTexture final
	{
	public:
		StagingTexture();
		StagingTexture(StagingTexture&& move);
		StagingTexture(const StagingTexture&) = delete;
		~StagingTexture();

		StagingTexture& operator=(StagingTexture&& move);
		StagingTexture& operator=(const StagingTexture&) = delete;

		__fi bool IsValid() const { return m_staging_buffer.IsValid(); }
		__fi bool IsMapped() const { return m_staging_buffer.IsMapped(); }
		__fi const char* GetMappedPointer() const { return m_staging_buffer.GetMapPointer(); }
		__fi char* GetMappedPointer() { return m_staging_buffer.GetMapPointer(); }
		__fi u32 GetMappedStride() const { return m_map_stride; }
		__fi u32 GetWidth() const { return m_width; }
		__fi u32 GetHeight() const { return m_height; }

		bool Create(StagingBuffer::Type type, VkFormat format, u32 width, u32 height);
		void Destroy(bool defer = true);

		// Copies from the GPU texture object to the staging texture, which can be mapped/read by the CPU.
		// Both src_rect and dst_rect must be with within the bounds of the the specified textures.
		void CopyFromTexture(VkCommandBuffer command_buffer, Texture& src_texture, u32 src_x, u32 src_y, u32 src_layer,
			u32 src_level, u32 dst_x, u32 dst_y, u32 width, u32 height);
		void CopyFromTexture(Texture& src_texture, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 dst_x, u32 dst_y,
			u32 width, u32 height);

		// Wrapper for copying a whole layer of a texture to a readback texture.
		// Assumes that the level of src texture and this texture have the same dimensions.
		void CopyToTexture(VkCommandBuffer command_buffer, u32 src_x, u32 src_y, Texture& dst_texture, u32 dst_x, u32 dst_y,
			u32 dst_layer, u32 dst_level, u32 width, u32 height);
		void CopyToTexture(u32 src_x, u32 src_y, Texture& dst_texture, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
			u32 width, u32 height);

		// Flushes pending writes from the CPU to the GPU, and reads from the GPU to the CPU.
		// This may cause a command buffer flush depending on if one has occurred between the last
		// call to CopyFromTexture()/CopyToTexture() and the Flush() call.
		void Flush();

		// Reads the specified rectangle from the staging texture to out_ptr, with the specified stride
		// (length in bytes of each row). CopyFromTexture must be called first. The contents of any
		// texels outside of the rectangle used for CopyFromTexture is undefined.
		void ReadTexels(u32 src_x, u32 src_y, u32 width, u32 height, void* out_ptr, u32 out_stride);
		void ReadTexel(u32 x, u32 y, void* out_ptr);

		// Copies the texels from in_ptr to the staging texture, which can be read by the GPU, with the
		// specified stride (length in bytes of each row). After updating the staging texture with all
		// changes, call CopyToTexture() to update the GPU copy.
		void WriteTexels(u32 dst_x, u32 dst_y, u32 width, u32 height, const void* in_ptr, u32 in_stride);
		void WriteTexel(u32 x, u32 y, const void* in_ptr);

	private:
		void PrepareForAccess();

		StagingBuffer m_staging_buffer;
		u64 m_flush_fence_counter = 0;
		u32 m_width = 0;
		u32 m_height = 0;
		u32 m_texel_size = 0;
		u32 m_map_stride = 0;
		bool m_needs_flush = false;
	};
} // namespace Vulkan