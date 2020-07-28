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

void add_label(wxWindow* parent, wxSizer* sizer, const char* str, long style = wxALIGN_RIGHT | wxALIGN_CENTRE_HORIZONTAL, wxSizerFlags flags = wxSizerFlags().Right().DoubleBorder())
{
	auto* temp_text = new wxStaticText(parent, wxID_ANY, str, wxDefaultPosition, wxDefaultSize, style);
	sizer->Add(temp_text, flags);
}

void add_combo_box(wxWindow* parent, wxSizer* sizer, wxChoice*& choice, const std::vector<GSSetting>& s, wxSizerFlags flags = wxSizerFlags().Expand().Left())
{
	wxArrayString temp;
	add_settings_to_array_string(s, temp);
	choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, temp);
	sizer->Add(choice, flags);
}

void add_label_and_combo_box(wxWindow* parent, wxSizer* sizer, wxChoice*& choice, const char* str, const std::vector<GSSetting>& s)
{
	add_label(parent, sizer, str);
	add_combo_box(parent, sizer, choice, s);
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

	auto* hw_choice_grid = new wxGridSizer(2, 0, 0);

	add_label_and_combo_box(this, hw_choice_grid, m_res_select, "Internal Resolution:", theApp.m_gs_upscale_multiplier);
	add_label_and_combo_box(this, hw_choice_grid, m_anisotropic_select, "Anisotropic Filtering:", theApp.m_gs_max_anisotropy);
	add_label_and_combo_box(this, hw_choice_grid, m_dither_select, "Dithering (PgDn):", theApp.m_gs_dithering);
	add_label_and_combo_box(this, hw_choice_grid, m_mipmap_select, "Mipmapping (Insert):", theApp.m_gs_hw_mipmapping);
	add_label_and_combo_box(this, hw_choice_grid, m_crc_select, "CRC Hack Level:", theApp.m_gs_crc_level);
	add_label_and_combo_box(this, hw_choice_grid, m_blend_select, "Blending Accuracy:", theApp.m_gs_acc_blend_level);

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

	add_label(this, thread_box, "Extra Rendering threads:");
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
	add_label_and_combo_box(this, rend_hack_choice_grid, m_half_select, "Half Screen Fix:", theApp.m_gs_generic_list);
	add_label_and_combo_box(this, rend_hack_choice_grid, m_tri_select, "Trilinear Filtering:", theApp.m_gs_trifilter);

	// Skipdraw Range
	add_label(this, rend_hack_choice_grid, "Skipdraw Range:");
	auto* skip_box = new wxBoxSizer(wxHORIZONTAL);
	skip_x_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	skip_y_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 10000, 0);
	skip_box->Add(skip_x_spin);
	skip_box->Add(skip_y_spin);

	rend_hack_choice_grid->Add(skip_box);

	// Upscale Hacks:
	add_label_and_combo_box(this, upscale_hack_choice_grid, m_gs_offset_hack_select, "Half-Pixel Offset:", theApp.m_gs_offset_hack);
	add_label_and_combo_box(this, upscale_hack_choice_grid, m_round_hack_select, "Round Sprite:", theApp.m_gs_hack);

	// Texture Offsets
	add_label(this, upscale_hack_choice_grid, "Texture Offsets:");
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
	add_label(this, record_grid_box, "Resolution:");
	auto* res_box = new wxBoxSizer(wxHORIZONTAL);
	res_x_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 256, 8192, 640);
	res_y_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 256, 8192, 480);
	res_box->Add(res_x_spin);
	res_box->Add(res_y_spin);

	record_grid_box->Add(res_box);

	add_label(this, record_grid_box, "Saving Threads:");
	thread_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 32, 4);
	record_grid_box->Add(thread_spin);

	add_label(this, record_grid_box, "PNG Compression Level:");
	png_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 9, 1);
	record_grid_box->Add(png_spin);

	add_label(this, record_grid_box, "Output Directory:");
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
	auto* shader_box = new wxStaticBoxSizer(wxVERTICAL, this, "Custom Shader");

	tex_filter_check = new wxCheckBox(this, wxID_ANY, "Texture Filtering of Display");
	fxaa_check = new wxCheckBox(this, wxID_ANY, "FXAA Shader (PgUp)");

	shader_box->Add(tex_filter_check);
	shader_box->Add(fxaa_check);

	shade_boost_check = new wxCheckBox(this, wxID_ANY, "Enable Shade Boost");
	shader_box->Add(shade_boost_check);

	auto* shade_boost_box = new wxStaticBoxSizer(wxVERTICAL, this, "Shade Boost");
	auto* shader_boost_grid = new wxFlexGridSizer(2, 0, 0);

	add_label(this, shader_boost_grid, "Brightness:");
	sb_brightness_slider = new wxSlider(this, wxID_ANY, 50, 0, 100, wxDefaultPosition, wxSize(250, -1), wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
	shader_boost_grid->Add(sb_brightness_slider, wxSizerFlags().Expand().Shaped());


	add_label(this, shader_boost_grid, "Contrast:");
	sb_contrast_slider = new wxSlider(this, wxID_ANY, 50, 0, 100, wxDefaultPosition, wxSize(250, -1), wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
	shader_boost_grid->Add(sb_contrast_slider, wxSizerFlags().Centre().Expand());

	add_label(this, shader_boost_grid, "Saturation:");
	sb_saturation_slider = new wxSlider(this, wxID_ANY, 50, 0, 100, wxDefaultPosition, wxSize(250, -1), wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
	shader_boost_grid->Add(sb_saturation_slider, wxSizerFlags().Centre().Expand());

	shade_boost_box->Add(shader_boost_grid, wxSizerFlags().Centre());
	shader_box->Add(shade_boost_box, wxSizerFlags().Expand().Centre());

	ext_shader_check = new wxCheckBox(this, wxID_ANY, "Enable External Shader");
	shader_box->Add(ext_shader_check);

	auto* ext_shader_box = new wxStaticBoxSizer(wxVERTICAL, this, "External Shader (Home)");
	auto* ext_shader_grid = new wxFlexGridSizer(4, 0, 0);

	ext_shader_grid->Add(new wxStaticText(this, wxID_ANY, "GLSL fx File:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	glsl_select = new wxDirPickerCtrl(this, wxID_ANY);
	ext_shader_grid->Add(glsl_select, wxSizerFlags().Expand());

	ext_shader_grid->Add(new wxStaticText(this, wxID_ANY, "Config File:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), wxSizerFlags().Centre());
	config_select = new wxDirPickerCtrl(this, wxID_ANY);
	ext_shader_grid->Add(config_select, wxSizerFlags().Expand());

	ext_shader_box->Add(ext_shader_grid, wxSizerFlags().Centre());
	shader_box->Add(ext_shader_box, wxSizerFlags().Expand().Centre());

	// TV Shader
	auto* tv_box = new wxBoxSizer(wxHORIZONTAL);
	add_label_and_combo_box(this, tv_box, m_tv_select, "TV Shader:", theApp.m_gs_tv_shaders);
	shader_box->Add(tv_box);

	tab_box->Add(shader_box, wxSizerFlags().Centre().Expand());
	SetSizerAndFit(tab_box);
}

OSDTab::OSDTab(wxWindow* parent)
	: wxPanel(parent, wxID_ANY)
{
	auto* tab_box = new wxBoxSizer(wxVERTICAL);

	monitor_check = new wxCheckBox(this, wxID_ANY, "Enable Monitor");
	tab_box->Add(monitor_check);

	auto* font_box = new wxStaticBoxSizer(wxVERTICAL, this, "Font");
	auto* font_grid = new wxFlexGridSizer(2, 0, 0);

	add_label(this, font_grid, "Size:");
	size_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100, 25);
	font_grid->Add(size_spin, wxSizerFlags().Expand());

	add_label(this, font_grid, "Red:");
	red_slider = new wxSlider(this, wxID_ANY, 0, 0, 255, wxDefaultPosition, wxSize(250, -1), wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
	font_grid->Add(red_slider, wxSizerFlags().Expand().Shaped());

	add_label(this, font_grid, "Green:");
	green_slider = new wxSlider(this, wxID_ANY, 0, 0, 255, wxDefaultPosition, wxSize(250, -1), wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
	font_grid->Add(green_slider, wxSizerFlags().Expand().Shaped());

	add_label(this, font_grid, "Blue:");
	blue_slider = new wxSlider(this, wxID_ANY, 0, 0, 255, wxDefaultPosition, wxSize(250, -1), wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
	font_grid->Add(blue_slider, wxSizerFlags().Expand().Shaped());

	add_label(this, font_grid, "Opacity:");
	opacity_slider = new wxSlider(this, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(250, -1), wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
	font_grid->Add(opacity_slider, wxSizerFlags().Expand().Shaped());

	font_box->Add(font_grid, wxSizerFlags().Centre().Expand());
	tab_box->Add(font_box, wxSizerFlags().Centre());

	log_check = new wxCheckBox(this, wxID_ANY, "Enable Log");
	tab_box->Add(log_check);

	auto* log_box = new wxStaticBoxSizer(wxVERTICAL, this, "Log Messages");
	auto* log_grid = new wxFlexGridSizer(2, 0, 0);

	add_label(this, log_grid, "Timeout (seconds):");
	timeout_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 10, 4);
	log_grid->Add(timeout_spin);

	add_label(this, log_grid, "Max On-Screen Messages:");
	max_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 10, 2);
	log_grid->Add(max_spin);

	log_box->Add(log_grid, wxSizerFlags().Centre().Expand());
	tab_box->Add(log_box, wxSizerFlags().Centre());

	SetSizerAndFit(tab_box);
}

OGLTab::OGLTab(wxWindow* parent)
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

	add_label_and_combo_box(this, top_grid, m_renderer_select, "Renderer:", theApp.m_gs_renderers);

	add_label(this, top_grid, "Adapter:");
	wxArrayString m_adapter_str;
	//add_settings_to_array_string(theApp.m_gs_renderers, m_adapter_str);
	m_adapter_str.Add("Default");
	m_adapter_select = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_adapter_str);
	top_grid->Add(m_adapter_select, wxSizerFlags().Expand());
	m_adapter_select->Disable();

	add_label_and_combo_box(this, top_grid, m_interlace_select, "Interlacing(F5):", theApp.m_gs_interlace);
	add_label_and_combo_box(this, top_grid, m_texture_select, "Texture Filtering:", theApp.m_gs_bifilter);

	auto* book = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxSize(450, -1));

	m_renderer_panel = new RendererTab(book);
	m_hacks_panel = new HacksTab(book);
	m_rec_panel = new RecTab(book);
	m_post_panel = new PostTab(book);
	m_osd_panel = new OSDTab(book);
	m_ogl_panel = new OGLTab(book);
	m_debug_panel = new DebugTab(book);

	book->AddPage(m_renderer_panel, "Renderer", true);
	book->AddPage(m_hacks_panel, "Hacks");
	book->AddPage(m_post_panel, "Shader");
	book->AddPage(m_osd_panel, "OSD");
	book->AddPage(m_ogl_panel, "OGL");
	book->AddPage(m_rec_panel, "Recording");
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
