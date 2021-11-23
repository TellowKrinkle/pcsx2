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

#include "MTLDrawableFetcher.h"
#include "common/PersistentThread.h"

void MTLDrawableFetcher::Run(CAMetalLayer* _Nonnull layer)
{
	Threading::SetNameOfCurrentThread("MTLDrawable Fetch Thread");
	std::unique_lock<std::mutex> l(m_mtx);

	while (true)
	{
		while (m_running && m_drawable)
			m_cv.wait(l);
		if (!m_running)
			return;

		@autoreleasepool
		{
			l.unlock();
			id<CAMetalDrawable> drawable = [layer nextDrawable];
			l.lock();
			m_drawable = drawable;
		}
	}
}

void MTLDrawableFetcher::Start(CAMetalLayer* _Nonnull layer)
{
	if (m_running)
		return;
	m_running = true;
	m_thread = std::thread([](MTLDrawableFetcher* me, CAMetalLayer* layer){ me->Run(layer); }, this, layer);
}

void MTLDrawableFetcher::Stop()
{
	if (!m_running)
		return;

	{
		std::lock_guard<std::mutex> guard(m_mtx);
		m_running = false;
	}
	m_cv.notify_one();
	m_thread.join();
	m_drawable = nil;
}

_Nullable id<CAMetalDrawable> MTLDrawableFetcher::GetIfAvailable()
{
	ASSERT(m_running);
	id<CAMetalDrawable> ret;
	{
		std::lock_guard<std::mutex> guard(m_mtx);
		ret = m_drawable;
		m_drawable = nil;
	}
	if (ret)
		m_cv.notify_one();
	return ret;
}
