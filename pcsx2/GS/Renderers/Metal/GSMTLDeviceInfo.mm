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

#if ! __has_feature(objc_arc)
	#error "Compile this with -fobjc-arc"
#endif

#include "GSMTLDeviceInfo.h"

#ifdef __APPLE__

GSMTLDevice::GSMTLDevice(id<MTLDevice> dev)
	: dev(dev)
{
	shaders = [dev newDefaultLibrary];

	memset(&features, 0, sizeof(features));

	if (char* env = getenv("MTL_UNIFIED_MEMORY"))
		features.unified_memory = env[0] == '1' || env[0] == 'y' || env[0] == 'Y';
	else if (@available(macOS 10.15, iOS 13.0, *))
		features.unified_memory = [dev hasUnifiedMemory];
	else
		features.unified_memory = false;

	if (@available(macOS 10.15, iOS 13.0, *))
		if ([dev supportsFamily:MTLGPUFamilyMac2] || [dev supportsFamily:MTLGPUFamilyApple1])
			features.texture_swizzle = true;

	features.max_texsize = 8192;
	if ([dev supportsFeatureSet:MTLFeatureSet_macOS_GPUFamily1_v1])
		features.max_texsize = 16384;
	if (@available(macOS 10.15, iOS 13.0, *))
		if ([dev supportsFamily:MTLGPUFamilyApple3])
			features.max_texsize = 16384;
}

#endif // __APPLE__
