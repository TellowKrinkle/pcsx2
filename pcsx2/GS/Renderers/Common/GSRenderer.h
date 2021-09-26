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

#include "GS/GSState.h"
#include "GS/GSCapture.h"
#include <memory>

struct HostKeyEvent;

class GSRenderer : public GSState
{
	GSCapture m_capture;
	std::string m_snapshot;
	int m_shader;

	bool Merge(int field);

	bool m_shift_key;
	bool m_control_key;

protected:
	int m_dithering;
	int m_interlace;
	bool m_aa1;
	bool m_shaderfx;
	bool m_fxaa;
	bool m_shadeboost;
	bool m_texture_shuffle;
	GSVector2i m_real_size;

	virtual GSTexture* GetOutput(int i, int& y_offset) = 0;
	virtual GSTexture* GetFeedbackOutput() { return nullptr; }

public:
	std::unique_ptr<GSDevice> m_dev;

public:
	GSRenderer(std::unique_ptr<GSDevice> dev);
	virtual ~GSRenderer();

	virtual void Destroy();

	virtual const char* GetName() const = 0;

	virtual void VSync(int field);
	virtual bool MakeSnapshot(const std::string& path);
	virtual void KeyEvent(const HostKeyEvent& e);
	virtual bool CanUpscale() { return false; }
	virtual int GetUpscaleMultiplier() { return 1; }
	virtual GSVector2i GetCustomResolution() { return GSVector2i(0, 0); }
	GSVector2i GetInternalResolution();

	virtual bool BeginCapture(std::string& filename);
	virtual void EndCapture();

	void PurgePool();

	bool SaveSnapshotToMemory(u32 width, u32 height, std::vector<u32>* pixels);
};
