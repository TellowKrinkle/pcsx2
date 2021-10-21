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

#include "GS.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "common/Vulkan/Texture.h"
#include "common/Vulkan/StagingTexture.h"

class GSTextureVK final : public GSTexture
{
	Vulkan::Texture m_texture;
	Vulkan::StagingTexture m_staging_texture;

	GSVector4i m_map_area = GSVector4i::zero();
	u32 m_map_level = UINT32_MAX;

	VkFramebuffer m_framebuffer = VK_NULL_HANDLE;

	// linked framebuffer is combined with depth texture
	// list of color textures this depth texture is linked to or vice versa
	std::vector<std::pair<GSTextureVK*, VkFramebuffer>> m_linked_textures;

public:
	GSTextureVK(int type, Vulkan::Texture texture);
	GSTextureVK(int type, VkFormat format, Vulkan::StagingTexture staging_texture);
	~GSTextureVK() override;

	static std::unique_ptr<GSTextureVK> Create(int type, u32 width, u32 height, u32 levels, VkFormat format);

	__fi Vulkan::Texture& GetTexture() { return m_texture; }
	__fi Vulkan::StagingTexture& GetStagingTexture() { return m_staging_texture; }

	void* GetNativeHandle() const override;

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = NULL, int layer = 0) override;
	void Unmap() override;
	bool Save(const std::string& fn) override;
	bool Equal(GSTextureVK* tex);

	VkImage GetImage() const { return m_texture.GetImage(); }
	VkImageView GetView() const { return m_texture.GetView(); }
	void TransitionToLayout(VkImageLayout layout);

	/// Framebuffers are lazily allocated.
	VkFramebuffer GetFramebuffer();

	VkFramebuffer GetLinkedFramebuffer(GSTextureVK* depth_texture);

private:
	VkCommandBuffer GetCommandBufferForUpdate();
};
