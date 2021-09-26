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

#include "PrecompiledHeader.h"

#include <wx/ffile.h>

#include "common/Console.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "AppConfig.h"
#include "Host.h"
#include "HostDisplay.h"
#include "HostSettings.h"
#include "GS/GS.h"

#include "common/Assertions.h"
#include "Frontend/ImGuiManager.h"
#include "Frontend/OpenGLHostDisplay.h"
#ifdef _WIN32
#include "Frontend/D3D11HostDisplay.h"
#endif

#include "gui/App.h"
#include "gui/AppHost.h"

#include <atomic>
#include <mutex>

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
	const wxString full_path(Path::Combine(EmuFolders::Resources, wxString::FromUTF8(filename)));
	wxFFile file(full_path, L"rb");

	if (!file.IsOpened())
	{
		Console.Error("Failed to open resource file '%s'", filename);
		return std::nullopt;
	}

	const size_t size = file.Length();
	std::vector<u8> ret(size);
	if (!file.Read(ret.data(), size))
	{
		Console.Error("Failed to read resource file '%s'", filename);
		return std::nullopt;
	}

	return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
	const wxString full_path(Path::Combine(EmuFolders::Resources, wxString::FromUTF8(filename)));
	wxFFile file(full_path, L"rb");

	if (!file.IsOpened())
	{
		Console.Error("Failed to open resource file '%s'", filename);
		return std::nullopt;
	}

	const size_t size = file.Length();
	std::string ret;
	ret.resize(size);
	if (!file.Read(ret.data(), size))
	{
		Console.Error("Failed to read resource file '%s'", filename);
		return std::nullopt;
	}

	return ret;
}

std::string Host::GetStringSettingValue(const char* section, const char* key, const char* default_value /* = "" */)
{
	return default_value;
}

static std::unique_ptr<HostDisplay> s_host_display;
std::atomic_bool init_gspanel = true;

HostDisplay* Host::AcquireHostDisplay(HostDisplay::RenderAPI api)
{
	if (init_gspanel)
		sApp.OpenGsPanel();

	s_host_display = HostDisplay::CreateDisplayForAPI(api);
	if (!s_host_display)
		return nullptr;

	pxAssert(g_gs_window_info.type != WindowInfo::Type::Surfaceless);
	
	const std::string adapter(GSGetConfigString("adapter"));
	if (!s_host_display->CreateRenderDevice(g_gs_window_info, adapter, GSConfig.UseDebugDevice) ||
		!s_host_display->InitializeRenderDevice(StringUtil::wxStringToUTF8String(EmuFolders::Cache.ToString()), GSConfig.UseDebugDevice) ||
		!ImGuiManager::Initialize())
	{
		s_host_display->DestroyRenderSurface();
		s_host_display->DestroyRenderDevice();
		s_host_display.reset();
		return nullptr;
	}

	return s_host_display.get();
}

void Host::ReleaseHostDisplay()
{
	ImGuiManager::Shutdown();

	if (s_host_display)
	{
		s_host_display->DestroyRenderSurface();
		s_host_display->DestroyRenderDevice();
		s_host_display.reset();
	}

	if (init_gspanel)
		sApp.CloseGsPanel();
}

HostDisplay* Host::GetHostDisplay()
{
	return s_host_display.get();
}

void Host::BeginFrame()
{
	CheckForGSWindowResize();
	ImGuiManager::NewFrame();
}

bool Host::BeginPresentFrame(bool frame_skip)
{
	return s_host_display->BeginPresent(frame_skip);
}

void Host::EndPresentFrame()
{
	ImGuiManager::RenderOSD();
	s_host_display->EndPresent();
}

void Host::UpdateHostDisplay()
{
	// not used for wx
}

void Host::ResizeHostDisplay(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	// not used for wx (except for osd scale changes)
	ImGuiManager::WindowResized();
}

bool Host::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
	return default_value;
}

static std::atomic_bool s_gs_window_resized{false};
static std::mutex s_gs_window_resized_lock;
static int s_new_gs_window_width = 0;
static int s_new_gs_window_height = 0;

void Host::GSWindowResized(int width, int height)
{
	std::unique_lock lock(s_gs_window_resized_lock);
	s_new_gs_window_width = width;
	s_new_gs_window_height = height;
	s_gs_window_resized.store(true);
}

void Host::CheckForGSWindowResize()
{
	if (!s_gs_window_resized.load())
		return;

	int width, height;
	{
		std::unique_lock lock(s_gs_window_resized_lock);
		width = s_new_gs_window_width;
		height = s_new_gs_window_height;
		s_gs_window_resized.store(false);
	}

	if (!s_host_display)
		return;

	s_host_display->ResizeRenderWindow(width, height, s_host_display ? s_host_display->GetWindowScale() : 1.0f);
	ImGuiManager::WindowResized();
}
