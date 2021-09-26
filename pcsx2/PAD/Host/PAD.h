/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#include "Global.h"
#include "Host.h"
#include "SaveState.h"

class SettingsInterface;
struct HostKeyEvent;
struct WindowInfo;

s32 PADinit();
void PADshutdown();
s32 PADopen(const WindowInfo& wi);
void PADclose();
s32 PADsetSlot(u8 port, u8 slot);
s32 PADfreeze(FreezeAction mode, freezeData* data);
u8 PADstartPoll(int pad);
u8 PADpoll(u8 value);

namespace PAD
{
	/// Sets default configuration.
	void SetDefaultConfig(SettingsInterface& si);

	/// Reloads configuration.
	void LoadConfig(const SettingsInterface& si);

	/// Called at guest vsync (input poll time)
	void PollDevices();

	/// Returns true if the event was consumed by the pad.
	bool HandleHostInputEvent(const HostKeyEvent& event);
} // namespace PAD
