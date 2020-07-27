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

size_t get_config_index(const std::vector<GSSetting>& s, const char* str)
{
	int value = theApp.GetConfigI(str);

	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i].value == value)
			return i;
	}
	return 0;
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

	hw_choice_grid->Add(m_res_select, wxSizerFlags().Expand());

	// Anisotropic Filtering
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Anisotropic Filtering:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_ani_str;
	add_settings_to_array_string(theApp.m_gs_max_anisotropy, m_ani_str);
	m_anisotropic_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_ani_str);

	hw_choice_grid->Add(m_anisotropic_select, wxSizerFlags().Expand());

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

	hw_choice_grid->Add(m_mipmap_select, wxSizerFlags().Expand());

	// CRC Hack Level
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "CRC Hack Level:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_crc_str;
	add_settings_to_array_string(theApp.m_gs_crc_level, m_crc_str);
	m_crc_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_crc_str);

	hw_choice_grid->Add(m_crc_select, wxSizerFlags().Expand());

	// Date Accuracy
	hw_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Blending Accuracy:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_blend_str;
	add_settings_to_array_string(theApp.m_gs_acc_blend_level, m_blend_str);
	m_blend_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_blend_str);

	hw_choice_grid->Add(m_blend_select, wxSizerFlags().Expand());

	auto* top_checks_box = new wxWrapSizer(wxHORIZONTAL);
	top_checks_box->Add(eight_bit_check, wxSizerFlags().Centre());
	top_checks_box->Add(framebuffer_check, wxSizerFlags().Centre());
	top_checks_box->Add(acc_date_check, wxSizerFlags().Centre());

	hardware_box->Add(top_checks_box, wxSizerFlags().Centre());
	hardware_box->Add(hw_choice_grid, wxSizerFlags().Centre());

	auto* bottom_checks_box = new wxWrapSizer(wxHORIZONTAL);
	flush_check = new wxCheckBox(this, wxID_ANY, "Auto Flush");
	edge_check = new wxCheckBox(this, wxID_ANY, "Edge Antialiasing (Del)");
	mipmap_check = new wxCheckBox(this, wxID_ANY, "Mipmapping");

	bottom_checks_box->Add(flush_check, wxSizerFlags().Centre());
	bottom_checks_box->Add(edge_check, wxSizerFlags().Centre());
	bottom_checks_box->Add(mipmap_check, wxSizerFlags().Centre());
	software_box->Add(bottom_checks_box, wxSizerFlags().Centre());

	// Rendering threads
	auto* thread_box = new wxBoxSizer(wxHORIZONTAL);
	thread_box->Add(new wxStaticText(this, wxID_ANY, "Extra Rendering threads:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	thread_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 32, 2);
	thread_box->Add(thread_spin, wxSizerFlags().Centre());
	software_box->Add(thread_box, wxSizerFlags().Centre());

	tab_box->Add(hardware_box, wxSizerFlags().Centre().Expand());
	tab_box->Add(software_box, wxSizerFlags().Centre().Expand());

	SetSizerAndFit(tab_box);
}

HacksTab::HacksTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);

	hacks_check = new wxCheckBox(this, wxID_ANY, "Enable User Hacks");

	auto* rend_hacks_box = new wxStaticBoxSizer(wxVERTICAL, this, "Renderer Hacks");
	auto* upscale_hacks_box = new wxStaticBoxSizer(wxVERTICAL, this, "Upscale Hacks");

	auto* rend_hacks_grid = new wxFlexGridSizer(2, 0, 0);
	auto* upscale_hacks_grid = new wxFlexGridSizer(3, 0, 0);

	// Renderer Hacks
	auto_flush_check = new wxCheckBox(this, wxID_ANY, "Auto Flush");
	fast_inv_check = new wxCheckBox(this, wxID_ANY, "Fast Texture Invalidation");
	dis_depth_check = new wxCheckBox(this, wxID_ANY, "Disable Depth Emulation");
	fb_convert_check = new wxCheckBox(this, wxID_ANY, "Frame Buffer Conversion");
	dis_safe_features_check = new wxCheckBox(this, wxID_ANY, "Disable Safe Features");
	mem_wrap_check = new wxCheckBox(this, wxID_ANY, "Memory Wrapping");
	preload_gs_check = new wxCheckBox(this, wxID_ANY, "Preload Frame Data");

	// Upscale
	align_sprite_check = new wxCheckBox(this, wxID_ANY, "Align Sprite");
	merge_sprite_check = new wxCheckBox(this, wxID_ANY, "Merge Sprite");
	wild_arms_check = new wxCheckBox(this, wxID_ANY, "Wild Arms Hack");

	rend_hacks_grid->Add(auto_flush_check);
	rend_hacks_grid->Add(fast_inv_check);
	rend_hacks_grid->Add(dis_depth_check);
	rend_hacks_grid->Add(fb_convert_check);
	rend_hacks_grid->Add(dis_safe_features_check);
	rend_hacks_grid->Add(mem_wrap_check);
	rend_hacks_grid->Add(preload_gs_check);

	upscale_hacks_grid->Add(align_sprite_check);
	upscale_hacks_grid->Add(merge_sprite_check);
	upscale_hacks_grid->Add(wild_arms_check);

	auto* rend_hack_choice_grid = new wxFlexGridSizer(2, 0, 0);
	auto* upscale_hack_choice_grid = new wxFlexGridSizer(2, 0, 0);

	// Renderer Hacks:
	// Half Screen Fix
	rend_hack_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Half Screen Fix:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_half_str;
	add_settings_to_array_string(theApp.m_gs_generic_list, m_half_str);
	m_half_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_half_str);

	rend_hack_choice_grid->Add(m_half_select, wxSizerFlags().Expand());

	// Trilinear Filtering
	rend_hack_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Trilinear Filtering:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_tri_str;
	add_settings_to_array_string(theApp.m_gs_trifilter, m_tri_str);
	m_tri_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_tri_str);

	rend_hack_choice_grid->Add(m_tri_select, wxSizerFlags().Expand());

	// Skipdraw Range
	rend_hack_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Skipdraw Range:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	auto* skip_box = new wxBoxSizer(wxHORIZONTAL);
	skip_x_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	skip_y_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	skip_box->Add(skip_x_spin);
	skip_box->Add(skip_y_spin);

	rend_hack_choice_grid->Add(skip_box);

	// Upscale Hacks:
	// Half-Pixel Offset
	upscale_hack_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Half-Pixel Offset:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_half_pixel_str;
	add_settings_to_array_string(theApp.m_gs_offset_hack, m_half_pixel_str);
	m_gs_offset_hack_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_half_pixel_str);

	upscale_hack_choice_grid->Add(m_gs_offset_hack_select, wxSizerFlags().Expand());

	// Round Sprite
	upscale_hack_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Round Sprite:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());

	wxArrayString m_round_str;
	add_settings_to_array_string(theApp.m_gs_hack, m_round_str);
	m_round_hack_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_round_str);

	upscale_hack_choice_grid->Add(m_round_hack_select, wxSizerFlags().Expand());


	// Texture Offsets
	upscale_hack_choice_grid->Add(new wxStaticText(this, wxID_ANY, "Texture Offsets:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	auto* tex_off_box = new wxBoxSizer(wxHORIZONTAL);
	tex_off_x_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	tex_off_y_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	tex_off_box->Add(tex_off_x_spin);
	tex_off_box->Add(tex_off_y_spin);

	upscale_hack_choice_grid->Add(tex_off_box);

	rend_hacks_box->Add(rend_hacks_grid);
	rend_hacks_box->Add(new wxStaticLine(this, wxID_ANY), wxSizerFlags().Centre().Expand().Border(5));
	rend_hacks_box->Add(rend_hack_choice_grid, wxSizerFlags().Expand());

	upscale_hacks_box->Add(upscale_hacks_grid);
	upscale_hacks_box->Add(new wxStaticLine(this, wxID_ANY), wxSizerFlags().Centre().Expand().Border(5));
	upscale_hacks_box->Add(upscale_hack_choice_grid, wxSizerFlags().Expand());

	tab_box->Add(hacks_check, wxSizerFlags().Left());
	tab_box->Add(rend_hacks_box, wxSizerFlags().Centre().Expand());
	tab_box->Add(upscale_hacks_box, wxSizerFlags().Centre().Expand());

	SetSizerAndFit(tab_box);
}

RecTab::RecTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);

	record_check = new wxCheckBox(this, wxID_ANY, "Enable Recording (F12)");
	auto* record_box = new wxStaticBoxSizer(wxVERTICAL, this, "Recording");
	auto* record_grid_box = new wxFlexGridSizer(2, 0, 0);

	// Resolution
	record_grid_box->Add(new wxStaticText(this, wxID_ANY, "Resolution:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	auto* res_box = new wxBoxSizer(wxHORIZONTAL);
	res_x_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 256, 8192, 640);
	res_y_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 256, 8192, 480);
	res_box->Add(res_x_spin);
	res_box->Add(res_y_spin);

	record_grid_box->Add(res_box);

	record_grid_box->Add(new wxStaticText(this, wxID_ANY, "Saving Threads:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	thread_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 32, 4);
	record_grid_box->Add(thread_spin);

	record_grid_box->Add(new wxStaticText(this, wxID_ANY, "PNG Compression Level:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	png_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 9, 1);
	record_grid_box->Add(png_spin);

	record_grid_box->Add(new wxStaticText(this, wxID_ANY, "Output Directory:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	dir_select = new wxDirPickerCtrl(this, wxID_ANY);
	record_grid_box->Add(dir_select, wxSizerFlags().Expand());

	record_box->Add(record_grid_box, wxSizerFlags().Centre().Expand());

	tab_box->Add(record_check, wxSizerFlags().Left());
	tab_box->Add(record_box, wxSizerFlags().Centre().Expand());
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

DebugTab::DebugTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);

	auto* debug_box = new wxStaticBoxSizer(wxVERTICAL, this, "Debug");
	debug_box->Add(new wxStaticText(this, wxID_ANY, "This is a test"), wxSizerFlags().Centre());
	tab_box->Add(debug_box, wxSizerFlags().Centre().Expand());

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
	m_top_box->Add(new wxStaticBitmap(this, wxID_ANY, logo), wxSizerFlags().Centre());

	auto* top_grid = new wxFlexGridSizer(2, 0, 0);
	top_grid->SetFlexibleDirection(wxHORIZONTAL);

	top_grid->Add(new wxStaticText(this, wxID_ANY, "Renderer:"), wxSizerFlags().Centre());

	wxArrayString m_renderer_str;
	add_settings_to_array_string(theApp.m_gs_renderers, m_renderer_str);
	m_renderer_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_renderer_str);
	top_grid->Add(m_renderer_select, wxSizerFlags().Expand());

	top_grid->Add(new wxStaticText(this, wxID_ANY, "Adapter:"), wxSizerFlags().Centre());
	wxArrayString m_adapter_str;
	//add_settings_to_array_string(theApp.m_gs_renderers, m_adapter_str);
	m_adapter_str.Add("Default");
	m_adapter_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_adapter_str);
	top_grid->Add(m_adapter_select, wxSizerFlags().Expand());
	m_adapter_select->Disable();

	top_grid->Add(new wxStaticText(this, wxID_ANY, "Interlacing(F5):"), wxSizerFlags().Centre());
	wxArrayString m_interlace_str;
	add_settings_to_array_string(theApp.m_gs_interlace, m_interlace_str);
	m_interlace_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_interlace_str);
	top_grid->Add(m_interlace_select, wxSizerFlags().Expand());

	top_grid->Add(new wxStaticText(this, wxID_ANY, "Texture Filtering:"), wxSizerFlags().Centre());
	wxArrayString m_texture_str;
	add_settings_to_array_string(theApp.m_gs_bifilter, m_texture_str);
	m_texture_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_texture_str);
	top_grid->Add(m_texture_select, wxSizerFlags().Expand());

	auto* book = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxSize(450, -1));

	m_renderer_panel = new RendererTab(book);
	m_hacks_panel = new HacksTab(book);
	m_rec_panel = new RecTab(book);
	m_post_panel = new PostTab(book);
	m_osd_panel = new OSDTab(book);
	m_debug_panel = new DebugTab(book);

	book->AddPage(m_renderer_panel, "Renderer", true);
	book->AddPage(m_hacks_panel, "Hacks");
	book->AddPage(m_rec_panel, "Recording");
	book->AddPage(m_post_panel, "Shader/OGL");
	book->AddPage(m_osd_panel, "OSD");
	book->AddPage(m_debug_panel, "Debug");
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
	m_renderer_select->SetSelection(get_config_index(theApp.m_gs_renderers, "Renderer"));
	m_adapter_select->SetSelection(0);
	m_interlace_select->SetSelection(get_config_index(theApp.m_gs_interlace, "interlace"));
	m_texture_select->SetSelection(get_config_index(theApp.m_gs_bifilter, "filter"));

	m_hacks_panel->hacks_check->SetValue(theApp.GetConfigB("UserHacks"));

	m_renderer_panel->acc_date_check->SetValue(theApp.GetConfigB("accurate_date"));
	m_renderer_panel->eight_bit_check->SetValue(theApp.GetConfigB("paltex"));
	m_renderer_panel->framebuffer_check->SetValue(theApp.GetConfigB("conservative_framebuffer"));
	m_renderer_panel->flush_check->SetValue(theApp.GetConfigB("autoflush_sw"));
	m_renderer_panel->edge_check->SetValue(theApp.GetConfigB("aa1"));
	m_renderer_panel->mipmap_check->SetValue(theApp.GetConfigB("mipmap"));

	m_renderer_panel->m_res_select->SetSelection(get_config_index(theApp.m_gs_upscale_multiplier, "upscale_multiplier"));
	m_renderer_panel->m_anisotropic_select->SetSelection(get_config_index(theApp.m_gs_max_anisotropy, "MaxAnisotropy"));
	m_renderer_panel->m_dither_select->SetSelection(get_config_index(theApp.m_gs_dithering, "dithering_ps2"));
	m_renderer_panel->m_mipmap_select->SetSelection(get_config_index(theApp.m_gs_hw_mipmapping, "mipmap_hw"));
	m_renderer_panel->m_crc_select->SetSelection(get_config_index(theApp.m_gs_crc_level, "crc_hack_level"));
	m_renderer_panel->m_blend_select->SetSelection(get_config_index(theApp.m_gs_acc_blend_level, "accurate_blending_unit"));
	m_renderer_panel->thread_spin->SetValue(theApp.GetConfigI("extrathreads"));
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
