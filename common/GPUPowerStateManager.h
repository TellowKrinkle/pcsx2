/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include "Pcsx2Defs.h"
#include <string_view>
#include <memory>

class GPUPowerStateManager
{
public:
	class Setter {
	public:
		static Setter* CreateForGPU(std::string_view gpu_name);
		virtual ~Setter();
		virtual void SetForceHighPowerState(bool forced) = 0;
	};

private:
	std::unique_ptr<Setter> setter;
	u32 high_frames_remaining = 0;

	GPUPowerStateManager() = default;

public:
	void SetUpForGPU(std::string_view gpu_name)
	{
		setter.reset(Setter::CreateForGPU(gpu_name));
	}
	void Reset() { setter.reset(); }
	void DisableForcedHighPowerState()
	{
		if (!high_frames_remaining)
			return;
		high_frames_remaining = 0;
		setter->SetForceHighPowerState(false);
	}
	void EnableForcedHighPowerState(u32 frames = 30)
	{
		if (!setter || frames <= high_frames_remaining)
			return;
		u32 old = high_frames_remaining;
		high_frames_remaining = frames;
		if (!old)
			setter->SetForceHighPowerState(true);
	}
	void FramePassed()
	{
		if (high_frames_remaining)
		{
			--high_frames_remaining;
			if (!high_frames_remaining)
				setter->SetForceHighPowerState(false);
		}
	}

	static GPUPowerStateManager shared;
};
