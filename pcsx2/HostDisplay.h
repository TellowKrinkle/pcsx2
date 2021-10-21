#pragma once

#include "common/Pcsx2Defs.h"
#include "common/WindowInfo.h"

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "Host.h"
#include "Config.h"

// An abstracted RGBA8 texture.
class HostDisplayTexture
{
public:
	virtual ~HostDisplayTexture();

	virtual void* GetHandle() const = 0;
	virtual u32 GetWidth() const = 0;
	virtual u32 GetHeight() const = 0;
	virtual u32 GetLayers() const = 0;
	virtual u32 GetLevels() const = 0;
	virtual u32 GetSamples() const = 0;
};

// Interface to the frontend's renderer.
class HostDisplay
{
public:
	enum class RenderAPI
	{
		None,
		D3D11,
		D3D12,
		Vulkan,
		OpenGL,
		OpenGLES
	};

	enum class Alignment
	{
		LeftOrTop,
		Center,
		RightOrBottom
	};

	struct AdapterAndModeList
	{
		std::vector<std::string> adapter_names;
		std::vector<std::string> fullscreen_modes;
	};

	virtual ~HostDisplay();

	/// Returns a string representing the specified API.
	static const char* RenderAPIToString(RenderAPI api);

	/// Creates a display for the specified API.
	static std::unique_ptr<HostDisplay> CreateDisplayForAPI(RenderAPI api);

	/// Parses a fullscreen mode into its components (width * height @ refresh hz)
	static bool ParseFullscreenMode(const std::string_view& mode, u32* width, u32* height, float* refresh_rate);

	/// Converts a fullscreen mode to a string.
	static std::string GetFullscreenModeString(u32 width, u32 height, float refresh_rate);

	/// Helper function for computing the draw rectangle in a larger window.
	static std::tuple<float, float, float, float> CalculateDrawRect(s32 window_width, s32 window_height,
		s32 texture_width, s32 texture_height, float display_aspect_ratio,
		bool integer_scaling, Alignment alignment);

	__fi const WindowInfo& GetWindowInfo() const { return m_window_info; }
	__fi s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
	__fi s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
	__fi float GetWindowScale() const { return m_window_info.surface_scale; }

	/// Changes the alignment for this display (screen positioning).
	__fi Alignment GetDisplayAlignment() const { return m_display_alignment; }
	__fi void SetDisplayAlignment(Alignment alignment) { m_display_alignment = alignment; }

	virtual RenderAPI GetRenderAPI() const = 0;
	virtual void* GetRenderDevice() const = 0;
	virtual void* GetRenderContext() const = 0;
	virtual void* GetRenderSurface() const = 0;

	virtual bool HasRenderDevice() const = 0;
	virtual bool HasRenderSurface() const = 0;

	virtual bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool threaded_presentation, bool debug_device) = 0;
	virtual bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device) = 0;
	virtual bool MakeRenderContextCurrent() = 0;
	virtual bool DoneRenderContextCurrent() = 0;
	virtual void DestroyRenderDevice() = 0;
	virtual void DestroyRenderSurface() = 0;
	virtual bool ChangeRenderWindow(const WindowInfo& wi) = 0;
	virtual bool SupportsFullscreen() const = 0;
	virtual bool IsFullscreen() = 0;
	virtual bool SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate) = 0;
	virtual AdapterAndModeList GetAdapterAndModeList() = 0;

	/// Call when the window size changes externally to recreate any resources.
	virtual void ResizeRenderWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) = 0;

	/// Creates an abstracted RGBA8 texture. If dynamic, the texture can be updated with UpdateTexture() below.
	virtual std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, const void* data,
		u32 data_stride, bool dynamic = false) = 0;
	virtual void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
		u32 data_stride) = 0;

	/// Returns false if the window was completely occluded. If frame_skip is set, the frame won't be
	/// displayed, but the GPU command queue will still be flushed.
	virtual bool BeginPresent(bool frame_skip) = 0;

	/// Presents the frame to the display, and renders OSD elements.
	virtual void EndPresent() = 0;

	/// Changes vsync mode for this display.
	virtual void SetVSync(VsyncMode mode) = 0;

	/// ImGui context management, usually called by derived classes.
	virtual bool CreateImGuiContext() = 0;
	virtual void DestroyImGuiContext() = 0;
	virtual bool UpdateImGuiFontTexture() = 0;

	/// Returns the effective refresh rate of this display.
	virtual bool GetHostRefreshRate(float* refresh_rate);

	/// Returns true if it's an OpenGL-based renderer.
	bool UsesLowerLeftOrigin() const;

	/// Limits the presentation rate, can improve fast forward performance in some drivers.
	void SetDisplayMaxFPS(float max_fps);
	bool ShouldSkipDisplayingFrame();

protected:
	WindowInfo m_window_info;

	u64 m_last_frame_displayed_time = 0;
	float m_display_frame_interval = 0.0f;

	Alignment m_display_alignment = Alignment::Center;
};

namespace Host
{
	/// Creates the host display. This may create a new window. The API used depends on the current configuration.
	HostDisplay* AcquireHostDisplay(HostDisplay::RenderAPI api);

	/// Destroys the host display. This may close the display window.
	void ReleaseHostDisplay();

	/// Returns a pointer to the current host display abstraction. Assumes AcquireHostDisplay() has been caled.
	HostDisplay* GetHostDisplay();

	/// Called by the MTGS at the start of a frame.
	void BeginFrame();

	/// Returns false if the window was completely occluded. If frame_skip is set, the frame won't be
	/// displayed, but the GPU command queue will still be flushed.
	bool BeginPresentFrame(bool frame_skip);

	/// Presents the frame to the display, and renders OSD elements.
	void EndPresentFrame();

	/// Called on the MTGS thread when a resize request is received.
	void ResizeHostDisplay(u32 new_window_width, u32 new_window_height, float new_window_scale);

	/// Called on the MTGS thread when a request to update the display is received.
	/// This could be a fullscreen transition, for example.
	void UpdateHostDisplay();
} // namespace Host