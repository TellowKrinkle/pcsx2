/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022 PCSX2 Dev Team
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

#ifndef __OBJC__
	#error "This header is for use with Objective-C++ only.
#endif

#ifdef __APPLE__

#include <Metal/Metal.h>

struct GSMTLDevice
{
	struct Features
	{
		bool unified_memory;
		bool texture_swizzle;
		int max_texsize;
	};

	id<MTLDevice> dev;
	id<MTLLibrary> shaders;
	Features features;

	GSMTLDevice() = default;
	explicit GSMTLDevice(id<MTLDevice> dev);

	bool IsOk() const { return dev && shaders; }
	void Reset()
	{
		dev = nullptr;
		shaders = nullptr;
	}
};

#endif // __APPLE__
