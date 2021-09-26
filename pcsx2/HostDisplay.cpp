#include "PrecompiledHeader.h"

#include "HostDisplay.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/StringUtil.h"
#include "common/Timer.h"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

HostDisplayTexture::~HostDisplayTexture() = default;

HostDisplay::~HostDisplay() = default;

const char* HostDisplay::RenderAPIToString(RenderAPI api)
{
	static const char* names[] = {"None", "D3D11", "D3D12", "Vulkan", "OpenGL", "OpenGLES"};
	return (static_cast<u32>(api) >= std::size(names)) ? names[0] : names[static_cast<u32>(api)];
}

bool HostDisplay::UsesLowerLeftOrigin() const
{
	const RenderAPI api = GetRenderAPI();
	return (api == RenderAPI::OpenGL || api == RenderAPI::OpenGLES);
}

void HostDisplay::SetDisplayMaxFPS(float max_fps)
{
	m_display_frame_interval = (max_fps > 0.0f) ? (1.0f / max_fps) : 0.0f;
}

bool HostDisplay::ShouldSkipDisplayingFrame()
{
	if (m_display_frame_interval == 0.0f)
		return false;

	const u64 now = Common::Timer::GetCurrentValue();
	const double diff = Common::Timer::ConvertValueToSeconds(now - m_last_frame_displayed_time);
	if (diff < m_display_frame_interval)
		return true;

	m_last_frame_displayed_time = now;
	return false;
}

bool HostDisplay::GetHostRefreshRate(float* refresh_rate)
{
	if (m_window_info.surface_refresh_rate > 0.0f)
	{
		*refresh_rate = m_window_info.surface_refresh_rate;
		return true;
	}

	return WindowInfo::QueryRefreshRateForWindow(m_window_info, refresh_rate);
}

bool HostDisplay::ParseFullscreenMode(const std::string_view& mode, u32* width, u32* height, float* refresh_rate)
{
	if (!mode.empty())
	{
		std::string_view::size_type sep1 = mode.find('x');
		if (sep1 != std::string_view::npos)
		{
			std::optional<u32> owidth = StringUtil::FromChars<u32>(mode.substr(0, sep1));
			sep1++;

			while (sep1 < mode.length() && std::isspace(mode[sep1]))
				sep1++;

			if (owidth.has_value() && sep1 < mode.length())
			{
				std::string_view::size_type sep2 = mode.find('@', sep1);
				if (sep2 != std::string_view::npos)
				{
					std::optional<u32> oheight = StringUtil::FromChars<u32>(mode.substr(sep1, sep2 - sep1));
					sep2++;

					while (sep2 < mode.length() && std::isspace(mode[sep2]))
						sep2++;

					if (oheight.has_value() && sep2 < mode.length())
					{
						std::optional<float> orefresh_rate = StringUtil::FromChars<float>(mode.substr(sep2));
						if (orefresh_rate.has_value())
						{
							*width = owidth.value();
							*height = oheight.value();
							*refresh_rate = orefresh_rate.value();
							return true;
						}
					}
				}
			}
		}
	}

	*width = 0;
	*height = 0;
	*refresh_rate = 0;
	return false;
}

std::string HostDisplay::GetFullscreenModeString(u32 width, u32 height, float refresh_rate)
{
	return StringUtil::StdStringFromFormat("%u x %u @ %f hz", width, height, refresh_rate);
}

std::tuple<float, float, float, float> HostDisplay::CalculateDrawRect(s32 window_width, s32 window_height,
	s32 texture_width, s32 texture_height, float display_aspect_ratio, bool integer_scaling, Alignment alignment)
{
	const float window_ratio = static_cast<float>(window_width) / static_cast<float>(window_height);
	const float x_scale = (display_aspect_ratio / (static_cast<float>(texture_width) / static_cast<float>(texture_height)));
	const float display_width = static_cast<float>(texture_width) * x_scale;
	const float display_height = static_cast<float>(texture_height);
	float left = 0.0f;
	float top = 0.0f;
	float width = display_width;
	float height = display_height;

	// now fit it within the window
	float scale;
	if ((display_width / display_height) >= window_ratio)
	{
		// align in middle vertically
		scale = static_cast<float>(window_width) / display_width;
		if (integer_scaling)
		{
			scale = std::max(std::floor(scale), 1.0f);
			left += std::max<float>((static_cast<float>(window_width) - display_width * scale) / 2.0f, 0.0f);
		}

		switch (alignment)
		{
			case Alignment::RightOrBottom:
				top += std::max<float>(static_cast<float>(window_height) - (display_height * scale), 0.0f);
				break;

			case Alignment::Center:
				top += std::max<float>((static_cast<float>(window_height) - (display_height * scale)) / 2.0f, 0.0f);
				break;

			case Alignment::LeftOrTop:
			default:
				break;
		}
	}
	else
	{
		// align in middle horizontally
		scale = static_cast<float>(window_height) / display_height;
		if (integer_scaling)
		{
			scale = std::max(std::floor(scale), 1.0f);
			top += std::max<float>((static_cast<float>(window_height) - (display_height * scale)) / 2.0f, 0.0f);
		}

		switch (alignment)
		{
			case Alignment::RightOrBottom:
				left += std::max<float>(static_cast<float>(window_width) - (display_width * scale), 0.0f);
				break;

			case Alignment::Center:
				left += std::max<float>((static_cast<float>(window_width) - (display_width * scale)) / 2.0f, 0.0f);
				break;

			case Alignment::LeftOrTop:
			default:
				break;
		}
	}

	width *= scale;
	height *= scale;

	return std::make_tuple(left, top, left + width, top + height);
}

#include "Frontend/OpenGLHostDisplay.h"

#ifdef _WIN32
#include "Frontend/D3D11HostDisplay.h"
#endif

std::unique_ptr<HostDisplay> HostDisplay::CreateDisplayForAPI(RenderAPI api)
{
	switch (api)
	{
#ifdef _WIN32
		case HostDisplay::RenderAPI::D3D11:
			return std::make_unique<D3D11HostDisplay>();
#endif

		case HostDisplay::RenderAPI::OpenGL:
		case HostDisplay::RenderAPI::OpenGLES:
			return std::make_unique<OpenGLHostDisplay>();

		default:
			Console.Error("Unknown render API %u", static_cast<unsigned>(api));
			return {};
	}
}
