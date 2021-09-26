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
#include "GSRenderer.h"
#include "GS/GSGL.h"
#include "Host.h"
#include "HostDisplay.h"
#include "pcsx2/Config.h"
#include "common/StringUtil.h"
#if defined(__unix__)
#include <X11/keysym.h>
#endif

const unsigned int s_interlace_nb = 8;
const unsigned int s_post_shader_nb = 5;
const unsigned int s_mipmap_nb = 3;

GSRenderer::GSRenderer(std::unique_ptr<GSDevice> dev)
	: m_shift_key(false)
	, m_control_key(false)
	, m_texture_shuffle(false)
	, m_real_size(0, 0)
	, m_dev(std::move(dev))
{
	m_interlace = theApp.GetConfigI("interlace") % s_interlace_nb;
	m_shader = theApp.GetConfigI("TVShader") % s_post_shader_nb;
	m_aa1 = theApp.GetConfigB("aa1");
	m_fxaa = theApp.GetConfigB("fxaa");
	m_shaderfx = theApp.GetConfigB("shaderfx");
	m_shadeboost = theApp.GetConfigB("ShadeBoost");
	m_dithering = theApp.GetConfigI("dithering_ps2"); // 0 off, 1 auto, 2 auto no scale
}

GSRenderer::~GSRenderer()
{
}

void GSRenderer::Destroy()
{
	m_dev->Destroy();
}

bool GSRenderer::Merge(int field)
{
	bool en[2];

	GSVector4i fr[2];
	GSVector4i dr[2];

	GSVector2i display_baseline = {INT_MAX, INT_MAX};
	GSVector2i frame_baseline = {INT_MAX, INT_MAX};

	for (int i = 0; i < 2; i++)
	{
		en[i] = IsEnabled(i);

		if (en[i])
		{
			fr[i] = GetFrameRect(i);
			dr[i] = GetDisplayRect(i);

			display_baseline.x = std::min(dr[i].left, display_baseline.x);
			display_baseline.y = std::min(dr[i].top, display_baseline.y);
			frame_baseline.x = std::min(fr[i].left, frame_baseline.x);
			frame_baseline.y = std::min(fr[i].top, frame_baseline.y);

			//printf("[%d]: %d %d %d %d, %d %d %d %d\n", i, fr[i].x,fr[i].y,fr[i].z,fr[i].w , dr[i].x,dr[i].y,dr[i].z,dr[i].w);
		}
	}

	if (!en[0] && !en[1])
	{
		return false;
	}

	GL_PUSH("Renderer Merge %d (0: enabled %d 0x%x, 1: enabled %d 0x%x)", s_n, en[0], m_regs->DISP[0].DISPFB.Block(), en[1], m_regs->DISP[1].DISPFB.Block());

	// try to avoid fullscreen blur, could be nice on tv but on a monitor it's like double vision, hurts my eyes (persona 4, guitar hero)
	//
	// NOTE: probably the technique explained in graphtip.pdf (Antialiasing by Supersampling / 4. Reading Odd/Even Scan Lines Separately with the PCRTC then Blending)

	bool samesrc =
		en[0] && en[1] &&
		m_regs->DISP[0].DISPFB.FBP == m_regs->DISP[1].DISPFB.FBP &&
		m_regs->DISP[0].DISPFB.FBW == m_regs->DISP[1].DISPFB.FBW &&
		m_regs->DISP[0].DISPFB.PSM == m_regs->DISP[1].DISPFB.PSM;

	if (samesrc /*&& m_regs->PMODE.SLBG == 0 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0x80*/)
	{
		// persona 4:
		//
		// fr[0] = 0 0 640 448
		// fr[1] = 0 1 640 448
		// dr[0] = 159 50 779 498
		// dr[1] = 159 50 779 497
		//
		// second image shifted up by 1 pixel and blended over itself
		//
		// god of war:
		//
		// fr[0] = 0 1 512 448
		// fr[1] = 0 0 512 448
		// dr[0] = 127 50 639 497
		// dr[1] = 127 50 639 498
		//
		// same just the first image shifted
		//
		// These kinds of cases are now fixed by the more generic frame_diff code below, as the code here was too specific and has become obsolete.
		// NOTE: Persona 4 and God Of War are not rare exceptions, many games have the same(or very similar) offsets.

		int topDiff = fr[0].top - fr[1].top;
		if (dr[0].eq(dr[1]) && (fr[0].eq(fr[1] + GSVector4i(0, topDiff, 0, topDiff)) || fr[1].eq(fr[0] + GSVector4i(0, topDiff, 0, topDiff))))
		{
			// dq5:
			//
			// fr[0] = 0 1 512 445
			// fr[1] = 0 0 512 444
			// dr[0] = 127 50 639 494
			// dr[1] = 127 50 639 494

			int top = std::min(fr[0].top, fr[1].top);
			int bottom = std::min(fr[0].bottom, fr[1].bottom);

			fr[0].top = fr[1].top = top;
			fr[0].bottom = fr[1].bottom = bottom;
		}
	}

	GSVector2i fs(0, 0);
	GSVector2i ds(0, 0);

	GSTexture* tex[3] = {NULL, NULL, NULL};
	int y_offset[3] = {0, 0, 0};

	s_n++;

	bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	if (samesrc && fr[0].bottom == fr[1].bottom && !feedback_merge)
	{
		tex[0] = GetOutput(0, y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
	}
	else
	{
		if (en[0])
			tex[0] = GetOutput(0, y_offset[0]);
		if (en[1])
			tex[1] = GetOutput(1, y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput();
	}

	GSVector4 src[2];
	GSVector4 src_hw[2];
	GSVector4 dst[2];

	for (int i = 0; i < 2; i++)
	{
		if (!en[i] || !tex[i])
			continue;

		GSVector4i r = fr[i];
		GSVector4 scale = GSVector4(tex[i]->GetScale()).xyxy();

		src[i] = GSVector4(r) * scale / GSVector4(tex[i]->GetSize()).xyxy();
		src_hw[i] = (GSVector4(r) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale / GSVector4(tex[i]->GetSize()).xyxy();

		GSVector2 off(0);
		GSVector2i display_diff(dr[i].left - display_baseline.x, dr[i].top - display_baseline.y);
		GSVector2i frame_diff(fr[i].left - frame_baseline.x, fr[i].top - frame_baseline.y);

		// Time Crisis 2/3 uses two side by side images when in split screen mode.
		// Though ignore cases where baseline and display rectangle offsets only differ by 1 pixel, causes blurring and wrong resolution output on FFXII
		if (display_diff.x > 2)
		{
			off.x = tex[i]->GetScale().x * display_diff.x;
		}
		// If the DX offset is too small then consider the status of frame memory offsets, prevents blurring on Tenchu: Fatal Shadows, Worms 3D
		else if (display_diff.x != frame_diff.x)
		{
			off.x = tex[i]->GetScale().x * frame_diff.x;
		}

		if (display_diff.y >= 4) // Shouldn't this be >= 2?
		{
			off.y = tex[i]->GetScale().y * display_diff.y;

			if (m_regs->SMODE2.INT && m_regs->SMODE2.FFMD)
			{
				off.y /= 2;
			}
		}
		else if (display_diff.y != frame_diff.y)
		{
			off.y = tex[i]->GetScale().y * frame_diff.y;
		}

		dst[i] = GSVector4(off).xyxy() + scale * GSVector4(r.rsize());

		fs.x = std::max(fs.x, (int)(dst[i].z + 0.5f));
		fs.y = std::max(fs.y, (int)(dst[i].w + 0.5f));
	}

	ds = fs;

	if (m_regs->SMODE2.INT && m_regs->SMODE2.FFMD)
	{
		ds.y *= 2;
	}
	m_real_size = ds;

	bool slbg = m_regs->PMODE.SLBG;

	if (tex[0] || tex[1])
	{
		if (tex[0] == tex[1] && !slbg && (src[0] == src[1] & dst[0] == dst[1]).alltrue())
		{
			// the two outputs are identical, skip drawing one of them (the one that is alpha blended)

			tex[0] = NULL;
		}

		GSVector4 c = GSVector4((int)m_regs->BGCOLOR.R, (int)m_regs->BGCOLOR.G, (int)m_regs->BGCOLOR.B, (int)m_regs->PMODE.ALP) / 255;

		m_dev->Merge(tex, src_hw, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

		if (m_regs->SMODE2.INT && m_interlace > 0)
		{
			if (m_interlace == 7 && m_regs->SMODE2.FFMD) // Auto interlace enabled / Odd frame interlace setting
			{
				int field2 = 0;
				int mode = 2;
				m_dev->Interlace(ds, field ^ field2, mode, tex[1] ? tex[1]->GetScale().y : tex[0]->GetScale().y);
			}
			else
			{
				int field2 = 1 - ((m_interlace - 1) & 1);
				int mode = (m_interlace - 1) >> 1;
				m_dev->Interlace(ds, field ^ field2, mode, tex[1] ? tex[1]->GetScale().y : tex[0]->GetScale().y);
			}
		}

		if (m_shadeboost)
		{
			m_dev->ShadeBoost();
		}

		if (m_shaderfx)
		{
			m_dev->ExternalFX();
		}

		if (m_fxaa)
		{
			m_dev->FXAA();
		}
	}

	return true;
}

GSVector2i GSRenderer::GetInternalResolution()
{
	return m_real_size;
}

static float GetCurrentAspectRatioFloat()
{
	static constexpr std::array<float, static_cast<size_t>(AspectRatioType::MaxCount)> ars = { {4.0f / 3.0f, 4.0f / 3.0f, 16.0f / 9.0f} };
	return ars[static_cast<u32>(GSConfig.AspectRatio)];
}

static GSVector4 CalculateDrawRect(s32 window_width, s32 window_height, s32 texture_width, s32 texture_height, HostDisplay::Alignment alignment, bool flip_y)
{
	GSVector4 ret;

	if (GSConfig.AspectRatio != AspectRatioType::Stretch)
	{
		std::tie(ret.x, ret.y, ret.z, ret.w) = HostDisplay::CalculateDrawRect(
			window_width, window_height, texture_width, texture_height,
			GetCurrentAspectRatioFloat(), GSConfig.IntegerScaling, alignment);
	}
	else
	{
		ret = GSVector4(0.0f, 0.0f, static_cast<float>(window_width), static_cast<float>(window_height));
	}

	if (flip_y)
	{
		const float height = ret.w - ret.y;
		ret.y = static_cast<float>(window_height) - ret.w;
		ret.w = ret.y + height;
	}

	return ret;
}

void GSRenderer::VSync(int field)
{
	GSPerfMonAutoTimer pmat(&m_perfmon);

	m_perfmon.Put(GSPerfMon::Frame);

	Flush();

	if (s_dump && s_n >= s_saven)
	{
		m_regs->Dump(root_sw + format("%05d_f%lld_gs_reg.txt", s_n, m_perfmon.GetFrame()));
	}

	m_dev->AgePool();

	if ((m_perfmon.GetFrame() & 0x1f) == 0)
		m_perfmon.Update();

	if (!Merge(field ? 1 : 0) || m_frameskip)
	{
		m_dev->ResetAPIState();
		if (Host::BeginPresentFrame(m_frameskip))
			Host::EndPresentFrame();

		m_dev->RestoreAPIState();
		return;
	}

	m_dev->ResetAPIState();
	if (Host::BeginPresentFrame(false))
	{
		GSTexture* current = m_dev->GetCurrent();
		if (current)
		{
			HostDisplay* const display = m_dev->GetDisplay();
			const GSVector4 draw_rect(CalculateDrawRect(display->GetWindowWidth(), display->GetWindowHeight(),
				current->GetWidth(), current->GetHeight(), display->GetDisplayAlignment(), display->UsesLowerLeftOrigin()));

			static constexpr int s_shader[5] = {ShaderConvert_COPY, ShaderConvert_SCANLINE,
				ShaderConvert_DIAGONAL_FILTER, ShaderConvert_TRIANGULAR_FILTER,
				ShaderConvert_COMPLEX_FILTER}; // FIXME

			m_dev->StretchRect(current, nullptr, draw_rect, s_shader[m_shader], GSConfig.LinearPresent);
		}

		Host::EndPresentFrame();
	}
	m_dev->RestoreAPIState();

	// snapshot

	if (!m_snapshot.empty())
	{
		if (!m_dump && m_shift_key)
		{
			freezeData fd = {0, nullptr};
			Freeze(&fd, true);
			fd.data = new u8[fd.size];
			Freeze(&fd, false);

			if (m_control_key)
				m_dump = std::unique_ptr<GSDumpBase>(new GSDump(m_snapshot, m_crc, fd, m_regs));
			else
				m_dump = std::unique_ptr<GSDumpBase>(new GSDumpXz(m_snapshot, m_crc, fd, m_regs));

			delete[] fd.data;
		}

		if (GSTexture* t = m_dev->GetCurrent())
		{
			t->Save(m_snapshot + ".png");
		}

		m_snapshot.clear();
	}
	else if (m_dump)
	{
		if (m_dump->VSync(field, !m_control_key, m_regs))
			m_dump.reset();
	}

	// capture

	if (m_capture.IsCapturing())
	{
		if (GSTexture* current = m_dev->GetCurrent())
		{
			GSVector2i size = m_capture.GetSize();

			if (GSTexture* offscreen = m_dev->CopyOffscreen(current, GSVector4(0, 0, 1, 1), size.x, size.y))
			{
				GSTexture::GSMap m;

				if (offscreen->Map(m))
				{
					m_capture.DeliverFrame(m.bits, m.pitch, !m_dev->IsRBSwapped());

					offscreen->Unmap();
				}

				m_dev->Recycle(offscreen);
			}
		}
	}
}

bool GSRenderer::MakeSnapshot(const std::string& path)
{
	if (m_snapshot.empty())
	{
		// Allows for providing a complete path
		if (path.substr(path.size() - 4, 4) == ".png")
			m_snapshot = path.substr(0, path.size() - 4);
		else
		{
			time_t cur_time = time(nullptr);
			static time_t prev_snap;
			// The variable 'n' is used for labelling the screenshots when multiple screenshots are taken in
			// a single second, we'll start using this variable for naming when a second screenshot request is detected
			// at the same time as the first one. Hence, we're initially setting this counter to 2 to imply that
			// the captured image is the 2nd image captured at this specific time.
			static int n = 2;
			char local_time[16];

			if (strftime(local_time, sizeof(local_time), "%Y%m%d%H%M%S", localtime(&cur_time)))
			{
				if (cur_time == prev_snap)
					m_snapshot = format("%s_%s_(%d)", path.c_str(), local_time, n++);
				else
				{
					n = 2;
					m_snapshot = format("%s_%s", path.c_str(), local_time);
				}
				prev_snap = cur_time;
			}
		}
	}

	return true;
}

bool GSRenderer::BeginCapture(std::string& filename)
{
	return m_capture.BeginCapture(GetTvRefreshRate(), GetInternalResolution(), GetCurrentAspectRatioFloat(), filename);
}

void GSRenderer::EndCapture()
{
	m_capture.EndCapture();
}

void GSRenderer::KeyEvent(const HostKeyEvent& e)
{
#ifndef __APPLE__ // TODO: Add hotkey support on macOS
#ifdef _WIN32
	m_shift_key = !!(::GetAsyncKeyState(VK_SHIFT) & 0x8000);
	m_control_key = !!(::GetAsyncKeyState(VK_CONTROL) & 0x8000);
#else
	switch (e.key)
	{
		case XK_Shift_L:
		case XK_Shift_R:
			m_shift_key = (e.type == HostKeyEvent::Type::KeyPressed);
			return;
		case XK_Control_L:
		case XK_Control_R:
			m_control_key = (e.type == HostKeyEvent::Type::KeyReleased);
			return;
	}
#endif

	if (e.type == HostKeyEvent::Type::KeyPressed)
	{

		int step = m_shift_key ? -1 : 1;

#if defined(__unix__)
#define VK_F5 XK_F5
#define VK_F6 XK_F6
#define VK_DELETE XK_Delete
#define VK_INSERT XK_Insert
#define VK_PRIOR XK_Prior
#define VK_NEXT XK_Next
#define VK_HOME XK_Home
#endif

		switch (e.key)
		{
			case VK_F5:
				m_interlace = (m_interlace + s_interlace_nb + step) % s_interlace_nb;
				theApp.SetConfig("interlace", m_interlace);
				printf("GS: Set deinterlace mode to %d (%s).\n", m_interlace, theApp.m_gs_interlace.at(m_interlace).name.c_str());
				return;
			case VK_DELETE:
				m_aa1 = !m_aa1;
				theApp.SetConfig("aa1", m_aa1);
				printf("GS: (Software) Edge anti-aliasing is now %s.\n", m_aa1 ? "enabled" : "disabled");
				return;
			case VK_INSERT:
				m_mipmap = (m_mipmap + s_mipmap_nb + step) % s_mipmap_nb;
				theApp.SetConfig("mipmap_hw", m_mipmap);
				printf("GS: Mipmapping is now %s.\n", theApp.m_gs_hack.at(m_mipmap).name.c_str());
				return;
			case VK_NEXT: // As requested by Prafull, to be removed later
				char dither_msg[3][16] = {"disabled", "auto", "auto unscaled"};
				m_dithering = (m_dithering + 1) % 3;
				printf("GS: Dithering is now %s.\n", dither_msg[m_dithering]);
				return;
		}
	}
#endif // __APPLE__
}

void GSRenderer::PurgePool()
{
	m_dev->PurgePool();
}

bool GSRenderer::SaveSnapshotToMemory(u32 width, u32 height, std::vector<u32>* pixels)
{
	GSTexture* const current = m_dev->GetCurrent();
	if (!current)
		return false;

	const GSVector4 draw_rect(CalculateDrawRect(width, height, current->GetWidth(), current->GetHeight(),
		HostDisplay::Alignment::LeftOrTop, false));
	const u32 draw_width = static_cast<u32>(draw_rect.z - draw_rect.x);
	const u32 draw_height = static_cast<u32>(draw_rect.w - draw_rect.y);
	pxAssertRel(draw_width <= width && draw_height <= height, "Draw rect is within bounds");

	GSTexture* const offscreen = m_dev->CopyOffscreen(
		current, GSVector4(0.0f, 0.0f, 1.0f, 1.0f),
		draw_width, draw_height);
	if (!offscreen)
		return false;

	GSTexture::GSMap map;
	const bool result = offscreen->Map(map);
	if (result)
	{
		const u32 pad_x = (width - draw_width) / 2;
		const u32 pad_y = (height - draw_height) / 2;
		pixels->resize(width * height, 0);
		StringUtil::StrideMemCpy(pixels->data() + pad_y * width + pad_x, width * sizeof(u32),
			map.bits, map.pitch, draw_width * sizeof(u32), draw_height);
		offscreen->Unmap();
	}

	m_dev->Recycle(offscreen);
	return result;
}
