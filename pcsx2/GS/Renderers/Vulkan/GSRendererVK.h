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

#include "GS/Renderers/HW/GSRendererHW.h"
#include "GSTextureCacheVK.h"
#include "GS/Renderers/HW/GSVertexHW.h"

class GSRendererVK final : public GSRendererHW
{
private:
	bool m_use_point_size;

private:
	__fi GSDeviceVK* GetDeviceVK() { return static_cast<GSDeviceVK*>(m_dev.get()); }

	void ResetStates();
	void SetupIA(const float& sx, const float& sy);
	void EmulateZbuffer();
	void EmulateBlending();
	void EmulateTextureShuffleAndFbmask();
	void EmulateChannelShuffle(GSTexture** rt, const GSTextureCache::Source* tex);
	void EmulateTextureSampler(const GSTextureCache::Source* tex);

	void SetBlendConstants(u8 afix);
	void ColorBufferBarrier(GSTexture* rt);
	void SendDraw(GSTexture* rt);

	GSDeviceVK::PipelineSelector m_p_sel;

	bool m_require_one_barrier = false;
	bool m_require_full_barrier = false;

	GSDeviceVK::PSConstantBuffer ps_cb;
	GSDeviceVK::VSConstantBuffer vs_cb;

public:
	GSRendererVK(std::unique_ptr<GSDevice> dev);
	virtual ~GSRendererVK() {}

	const char* GetName() const override;

	void DrawPrims(GSTexture* rt, GSTexture* ds, GSTextureCache::Source* tex) final;

	bool IsDummyTexture() const final;
};
