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
#include "GSwxDialog.h"

using namespace GSSettingsDialog;

void add_settings_to_array_string(const std::vector<GSSetting>& s, wxArrayString& arr)
{
	for (const GSSetting& setting : s)
	{
		if (!setting.note.empty())
			arr.Add(setting.name + " (" + setting.note + ")");
		else
			arr.Add(setting.name);
	}
}

RendererTab::RendererTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);
	auto* hardware_box = new wxStaticBoxSizer(wxVERTICAL, this, "Hardware Mode");
	auto* software_box = new wxStaticBoxSizer(wxVERTICAL, this, "Software Mode");

	eight_bit_check = new wxCheckBox(this, wxID_ANY, "GPU Palette Conversion");
	framebuffer_check = new wxCheckBox(this, wxID_ANY, "Conservative Buffer Allocation");
	acc_date_check = new wxCheckBox(this, wxID_ANY, "Accurate DATE");

	auto* hw_choice_grid = new wxFlexGridSizer(2, 0, 0);

	// Internal Resolution
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Internal Resolution:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_res_str;
	add_settings_to_array_string(theApp.m_gs_upscale_multiplier, m_res_str);
	m_res_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_res_str);

	hw_choice_grid->Add(m_res_select);

	// Anisotropic Filtering
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Anisotropic Filtering:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_ani_str;
	add_settings_to_array_string(theApp.m_gs_max_anisotropy, m_ani_str);
	m_anisotropic_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_ani_str);

	hw_choice_grid->Add(m_anisotropic_select);

	// Dithering
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Dithering (PgDn):", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_dither_str;
	add_settings_to_array_string(theApp.m_gs_dithering, m_dither_str);
	m_dither_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_dither_str);

	hw_choice_grid->Add(m_dither_select);

	// MipMapping
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Mipmapping (Insert):", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_mip_str;
	add_settings_to_array_string(theApp.m_gs_hw_mipmapping, m_mip_str);
	m_mipmap_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_mip_str);

	hw_choice_grid->Add(m_mipmap_select);

	// CRC Hack Level
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "CRC Hack Level:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_crc_str;
	add_settings_to_array_string(theApp.m_gs_crc_level, m_crc_str);
	m_crc_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_crc_str);

	hw_choice_grid->Add(m_crc_select);

	// Date Accuracy
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Blending Accuracy:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_blend_str;
	add_settings_to_array_string(theApp.m_gs_acc_blend_level, m_blend_str);
	m_blend_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_blend_str);

	hw_choice_grid->Add(m_blend_select);

	auto* top_checks_box = new wxWrapSizer(wxHORIZONTAL);
	top_checks_box->Add(eight_bit_check, wxSizerFlags().Centre());
	top_checks_box->Add(framebuffer_check, wxSizerFlags().Centre());
	top_checks_box->Add(acc_date_check, wxSizerFlags().Centre());

	hardware_box->Add(top_checks_box, wxSizerFlags().Centre());
	hardware_box->Add(hw_choice_grid, wxSizerFlags().Centre());

	// Rendering threads
	auto* thread_box = new wxBoxSizer(wxHORIZONTAL);
	thread_box->Add(new wxStaticText(this, wxID_ANY, "Extra Rendering threads:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	thread_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 32, 2);
	thread_box->Add(thread_spin, wxSizerFlags().Centre());
	software_box->Add(thread_box, wxSizerFlags().Centre());

	auto* bottom_checks_box = new wxWrapSizer(wxHORIZONTAL);
	flush_check = new wxCheckBox(this, wxID_ANY, "Auto Flush");
	edge_check = new wxCheckBox(this, wxID_ANY, "Edge Antialiasing (Del)");
	mipmap_check = new wxCheckBox(this, wxID_ANY, "Mipmapping");

	bottom_checks_box->Add(flush_check, wxSizerFlags().Centre());
	bottom_checks_box->Add(edge_check, wxSizerFlags().Centre());
	bottom_checks_box->Add(mipmap_check, wxSizerFlags().Centre());
	software_box->Add(bottom_checks_box, wxSizerFlags().Centre());

	tab_box->Add(hardware_box, wxSizerFlags().Centre().Expand());
	tab_box->Add(software_box, wxSizerFlags().Centre().Expand());


	SetSizerAndFit(tab_box);
}

HacksTab::HacksTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);

	hacks_check = new wxCheckBox(this, wxID_ANY, "Enable User Hacks");

	auto* hacks_box = new wxStaticBoxSizer(wxVERTICAL, this, "Hacks");

	auto* hack_checks_box = new wxFlexGridSizer(2, 0, 0);
	align_sprite_check = new wxCheckBox(this, wxID_ANY, "Align Sprite");
	fb_convert_check = new wxCheckBox(this, wxID_ANY, "Frame Buffer Conversion");
	auto_flush_check = new wxCheckBox(this, wxID_ANY, "Auto Flush");
	mem_wrap_check = new wxCheckBox(this, wxID_ANY, "Memory Wrapping");
	dis_depth_check = new wxCheckBox(this, wxID_ANY, "Disable Depth Emulation");
	merge_sprite_check = new wxCheckBox(this, wxID_ANY, "Merge Sprite");
	dis_safe_features_check = new wxCheckBox(this, wxID_ANY, "Disable Safe Features");
	preload_gs_check = new wxCheckBox(this, wxID_ANY, "Preload Frame Data");
	fast_inv_check = new wxCheckBox(this, wxID_ANY, "Fast Texture Invalidation");
	wild_arms_check = new wxCheckBox(this, wxID_ANY, "Wild Arms Hack");

	hack_checks_box->Add(align_sprite_check);
	hack_checks_box->Add(fb_convert_check);
	hack_checks_box->Add(auto_flush_check);
	hack_checks_box->Add(mem_wrap_check);
	hack_checks_box->Add(dis_depth_check);
	hack_checks_box->Add(merge_sprite_check);
	hack_checks_box->Add(dis_safe_features_check);
	hack_checks_box->Add(preload_gs_check);
	hack_checks_box->Add(fast_inv_check);
	hack_checks_box->Add(wild_arms_check);

	auto* hack_choice_box = new wxFlexGridSizer(2, 0, 0);

	// Half Screen Fix
	hack_choice_box->Add(new wxStaticText(this, wxID_ANY, "Half Screen Fix:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_half_str;
	add_settings_to_array_string(theApp.m_gs_generic_list, m_half_str);
	m_half_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_half_str);

	hack_choice_box->Add(m_half_select);

	// Trilinear Filtering
	hack_choice_box->Add(new wxStaticText(this, wxID_ANY, "Trilinear Filtering:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_tri_str;
	add_settings_to_array_string(theApp.m_gs_trifilter, m_tri_str);
	m_tri_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_tri_str);

	hack_choice_box->Add(m_tri_select);

	// Half-Pixel Offset
	hack_choice_box->Add(new wxStaticText(this, wxID_ANY, "Half-Pixel Offset:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_half_pixel_str;
	add_settings_to_array_string(theApp.m_gs_offset_hack, m_half_pixel_str);
	m_gs_offset_hack_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_half_pixel_str);

	hack_choice_box->Add(m_gs_offset_hack_select);

	// Round Sprite
	hack_choice_box->Add(new wxStaticText(this, wxID_ANY, "Round Sprite:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_round_str;
	add_settings_to_array_string(theApp.m_gs_hack, m_round_str);
	m_round_hack_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_round_str);

	hack_choice_box->Add(m_round_hack_select);

	// Skipdraw Range
	hack_choice_box->Add(new wxStaticText(this, wxID_ANY, "Skipdraw Range:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	auto* skip_box = new wxBoxSizer(wxHORIZONTAL);
	skip_x_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	skip_y_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	skip_box->Add(skip_x_spin);
	skip_box->Add(skip_y_spin);

	hack_choice_box->Add(skip_box);

	// Texture Offsets
	hack_choice_box->Add(new wxStaticText(this, wxID_ANY, "Texture Offsets:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	auto* tex_off_box = new wxBoxSizer(wxHORIZONTAL);
	tex_off_x_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	tex_off_y_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	tex_off_box->Add(tex_off_x_spin);
	tex_off_box->Add(tex_off_y_spin);

	hack_choice_box->Add(tex_off_box);

	hacks_box->Add(hack_checks_box);
	hacks_box->Add(new wxStaticLine(this, wxID_ANY), wxSizerFlags().Centre().Expand().Border(5));
	hacks_box->Add(hack_choice_box, wxSizerFlags().Expand());

	tab_box->Add(hacks_check, wxSizerFlags().Left());
	tab_box->Add(hacks_box, wxSizerFlags().Centre().Expand());

	SetSizerAndFit(tab_box);
}

DebugTab::DebugTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);
	tab_box->Add(new wxStaticText(this, wxID_ANY, "This is a test"), wxSizerFlags().Centre());

	SetSizerAndFit(tab_box);
}

PostTab::PostTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);
	auto* opengl_box = new wxStaticBoxSizer(wxVERTICAL, this, "Very Advanced OpenGL Settings");
	opengl_box->Add(new wxStaticText(this, wxID_ANY, "This is a test"), wxSizerFlags().Centre());

	tab_box->Add(opengl_box, wxSizerFlags().Centre().Expand());
	SetSizerAndFit(tab_box);
}

OSDTab::OSDTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);
	tab_box->Add(new wxStaticText(this, wxID_ANY, "This is a test"), wxSizerFlags().Centre());

	SetSizerAndFit(tab_box);
}

Dialog::Dialog()
	: wxDialog(nullptr, wxID_ANY, "Graphics Settings", wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
	auto* padding = new wxBoxSizer(wxVERTICAL);
	m_top_box = new wxBoxSizer(wxVERTICAL);

	// Right now, I'm hardcoding the location of the picture.
	wxString fileLocation = wxStandardPaths::Get().GetExecutablePath();
	fileLocation = wxFileName(fileLocation).GetPath() + wxT("/plugins/logo-ogl.bmp");

	wxBitmap logo(fileLocation, wxBITMAP_TYPE_BMP);

	wxStaticBitmap* image = new wxStaticBitmap(this, wxID_ANY, logo);
	m_top_box->Add(image, wxSizerFlags().Centre());

	auto* top_grid = new wxFlexGridSizer(2, 0, 0);

	top_grid->Add(new wxStaticText(this, wxID_ANY, "Renderer:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	add_settings_to_array_string(theApp.m_gs_renderers, m_renderer_str);
	m_renderer_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_renderer_str);
	top_grid->Add(m_renderer_select);

	top_grid->Add(new wxStaticText(this, wxID_ANY, "Interlacing(F5):", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	add_settings_to_array_string(theApp.m_gs_interlace, m_interlace_str);
	m_interlace_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_interlace_str);
	top_grid->Add(m_interlace_select);

	top_grid->Add(new wxStaticText(this, wxID_ANY, "Texture Filtering:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	add_settings_to_array_string(theApp.m_gs_bifilter, m_texture_str);
	m_texture_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_texture_str);
	top_grid->Add(m_texture_select);

	auto* book = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxSize(450, -1));

	m_renderer_panel = new RendererTab(book);
	m_hacks_panel = new HacksTab(book);
	m_debug_rec_panel = new DebugTab(book);
	m_post_panel = new PostTab(book);
	m_osd_panel = new OSDTab(book);

	book->AddPage(m_renderer_panel, "Renderer", true);
	book->AddPage(m_hacks_panel, "Hacks");
	book->AddPage(m_debug_rec_panel, "Debug/Recording");
	book->AddPage(m_post_panel, "Post Processing/OGL");
	book->AddPage(m_osd_panel, "OSD");
	book->SetPadding(wxSize(0, 0));

	m_top_box->Add(top_grid, wxSizerFlags().Centre());
	m_top_box->Add(book, wxSizerFlags().Centre().Expand());

	padding->Add(m_top_box, wxSizerFlags().Centre().Expand().Border(wxALL, 5));

	SetSizerAndFit(padding);
}

Dialog::~Dialog()
{
}

void Dialog::Reset()
{
}

void Dialog::Save()
{
}

bool RunwxDialog()
{
	Dialog GSSettingsDialog;

	GSSettingsDialog.Reset();
	GSSettingsDialog.ShowModal();
	GSSettingsDialog.Save();

	return true;
}
