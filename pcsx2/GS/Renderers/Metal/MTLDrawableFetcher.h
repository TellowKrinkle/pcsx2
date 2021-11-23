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

#ifndef __OBJC__
#error "This header is for use with Objective-C++ only.
#endif

#include <mutex>
#include <condition_variable>
#include <thread>
#include <QuartzCore/QuartzCore.h>

/// Asynchronously fetches drawables from a CAMetalLayer so we can avoid blocking
///
/// Metal only supports a blocking, queue-based flip system
/// When fullscreen, they allow an unlimited frame rate, but in windowed they throttle drawable requests to the refresh rate of everyone else
/// Since we tie frame rate to game speed, it's useful to be able to run faster than the display refresh rate
class MTLDrawableFetcher
{
	std::thread m_thread;
	std::mutex m_mtx;
	std::condition_variable m_cv;
	_Nullable id<CAMetalDrawable> m_drawable;
	bool m_running = false;
	void Run(CAMetalLayer* _Nonnull layer);

public:
	void Start(CAMetalLayer* _Nonnull layer);
	void Stop();
	_Nullable id<CAMetalDrawable> GetIfAvailable();
};
