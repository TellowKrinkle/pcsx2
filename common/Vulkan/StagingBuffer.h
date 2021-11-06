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
#include "common/Pcsx2Defs.h"
#include "common/Vulkan/Loader.h"
#include <memory>

namespace Vulkan
{
	class StagingBuffer
	{
	public:
		enum class Type
		{
			Upload,
			Readback,
			Mutable
		};

		StagingBuffer();
		StagingBuffer(StagingBuffer&& move);
		StagingBuffer(const StagingBuffer&) = delete;
		virtual ~StagingBuffer();

		StagingBuffer& operator=(StagingBuffer&& move);
		StagingBuffer& operator=(const StagingBuffer&) = delete;

		__fi Type GetType() const { return m_type; }
		__fi VkDeviceSize GetSize() const { return m_size; }
		__fi VkBuffer GetBuffer() const { return m_buffer; }
		__fi bool IsMapped() const { return m_map_pointer != nullptr; }
		__fi const char* GetMapPointer() const { return m_map_pointer; }
		__fi char* GetMapPointer() { return m_map_pointer; }
		__fi VkDeviceSize GetMapOffset() const { return m_map_offset; }
		__fi VkDeviceSize GetMapSize() const { return m_map_size; }
		__fi bool IsValid() const { return (m_buffer != VK_NULL_HANDLE); }
		__fi bool IsCoherent() const { return m_coherent; }

		bool Map(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
		void Unmap();

		// Upload part 1: Prepare from device read from the CPU side
		void FlushCPUCache(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

		// Upload part 2: Prepare for device read from the GPU side
		// Implicit when submitting the command buffer, so rarely needed.
		void InvalidateGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits dst_access_flags,
			VkPipelineStageFlagBits dst_pipeline_stage, VkDeviceSize offset = 0,
			VkDeviceSize size = VK_WHOLE_SIZE);

		// Readback part 0: Prepare for GPU usage (if necessary)
		void PrepareForGPUWrite(VkCommandBuffer command_buffer, VkAccessFlagBits dst_access_flags,
			VkPipelineStageFlagBits dst_pipeline_stage, VkDeviceSize offset = 0,
			VkDeviceSize size = VK_WHOLE_SIZE);

		// Readback part 1: Prepare for host readback from the GPU side
		void FlushGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits src_access_flags,
			VkPipelineStageFlagBits src_pipeline_stage, VkDeviceSize offset = 0,
			VkDeviceSize size = VK_WHOLE_SIZE);

		// Readback part 2: Prepare for host readback from the CPU side
		void InvalidateCPUCache(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

		// offset is from the start of the buffer, not from the map offset
		void Read(VkDeviceSize offset, void* data, size_t size, bool invalidate_caches = true);
		void Write(VkDeviceSize offset, const void* data, size_t size, bool invalidate_caches = true);

		// Creates the optimal format of image copy.
		bool Create(Type type, VkDeviceSize size, VkBufferUsageFlags usage);

		void Destroy(bool defer = true);

		// Allocates the resources needed to create a staging buffer.
		static bool AllocateBuffer(Type type, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* out_buffer,
			VkDeviceMemory* out_memory, bool* out_coherent);

	protected:
		Type m_type = Type::Upload;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		VkDeviceSize m_size = 0;
		bool m_coherent = false;

		char* m_map_pointer = nullptr;
		VkDeviceSize m_map_offset = 0;
		VkDeviceSize m_map_size = 0;
	};
} // namespace Vulkan
