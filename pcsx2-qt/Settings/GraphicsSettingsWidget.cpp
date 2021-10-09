#include "PrecompiledHeader.h"

#include "GraphicsSettingsWidget.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "SettingsDialog.h"
#include <QtWidgets/QMessageBox>

#include "pcsx2/GS/GS.h"

#ifdef _WIN32
#include "Frontend/D3D11HostDisplay.h"
#endif

struct RendererInfo
{
  const char* name;
  GSRendererType type;
};

static constexpr RendererInfo s_renderer_info[] = {
  QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "Automatic"), GSRendererType::Auto,
#ifdef _WIN32
  QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "Direct3D 11"), GSRendererType::DX11,
#endif
  QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "OpenGL"), GSRendererType::OGL,
  QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "Software"), GSRendererType::SW,
  QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "Null"), GSRendererType::Null,
};

GraphicsSettingsWidget::GraphicsSettingsWidget(QWidget* parent, SettingsDialog* dialog) : QWidget(parent)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToIntSetting(m_ui.vsync, "EmuCore/GS", "VsyncEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.useBlitSwapChain, "EmuCore/GS", "UseBlitSwapChain", false);
  SettingWidgetBinder::BindWidgetToStringSetting(m_ui.adapter, "EmuCore/GS", "Adapter");

  SettingWidgetBinder::BindWidgetToIntSetting(m_ui.upscaleMultiplier, "EmuCore/GS", "upscale_multiplier", 1);
  SettingWidgetBinder::BindWidgetToIntSetting(m_ui.blending, "EmuCore/GS", "accurate_blending_unit", 1);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.accurateDATE, "EmuCore/GS", "accurate_date", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.conservativeBufferAllocation, "EmuCore/GS", "conservative_framebuffer", true);
  SettingWidgetBinder::BindWidgetToIntSetting(m_ui.extraSWThreads, "EmuCore/GS", "extrathreads", 2);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.swAutoFlush, "EmuCore/GS", "autoflush_sw", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.gpuPaletteConversion, "EmuCore/GS", "paltex", false);

  const GSRendererType current_renderer = static_cast<GSRendererType>(QtHost::GetBaseIntSettingValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::Auto)));
  for (const RendererInfo& ri : s_renderer_info)
  {
    m_ui.renderer->addItem(qApp->translate("GraphicsSettingsWidget", ri.name));
    if (ri.type == current_renderer)
      m_ui.renderer->setCurrentIndex(m_ui.renderer->count() - 1);
  }
  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &GraphicsSettingsWidget::onRendererChanged);
  updateRendererDependentOptions();

  dialog->registerWidgetHelp(m_ui.useBlitSwapChain, tr("Use Blit Swap Chain"), tr("Unchecked"),
                             tr("Uses a blit presentation model instead of flipping when using the Direct3D 11 "
                                "renderer. This usually results in slower performance, but may be required for some "
                                "streaming applications, or to uncap framerates on some systems."));
}

GraphicsSettingsWidget::~GraphicsSettingsWidget() = default;

void GraphicsSettingsWidget::onRendererChanged(int index)
{
  QtHost::SetBaseIntSettingValue("EmuCore/GS", "Renderer", static_cast<int>(s_renderer_info[index].type));
  g_emu_thread->applySettings();
  updateRendererDependentOptions();
}

void GraphicsSettingsWidget::onAdapterChanged(int index)
{
  if (index == 0)
    QtHost::RemoveBaseSettingValue("EmuCore/GS", "Adapter");
  else
    QtHost::SetBaseStringSettingValue("EmuCore/GS", "Adapter", m_ui.adapter->currentText().toUtf8().constData());
  g_emu_thread->applySettings();
}

void GraphicsSettingsWidget::updateRendererDependentOptions()
{
  const int index = m_ui.renderer->currentIndex();
  GSRendererType type = s_renderer_info[index].type;
  if (type == GSRendererType::Auto)
    type = GSGetBestRenderer();

#ifdef _WIN32
  const bool is_dx11 = (type == GSRendererType::DX11 || type == GSRendererType::SW);
#else
  const bool is_dx11 = false;
#endif

  const bool is_hardware = (type == GSRendererType::DX11 || type == GSRendererType::OGL);
  const bool is_software = (type == GSRendererType::SW);

  if (m_hardware_renderer_visible != is_hardware)
  {
    m_ui.hardwareRendererGroup->setVisible(is_hardware);
    if (!is_hardware)
      m_ui.verticalLayout->removeWidget(m_ui.hardwareRendererGroup);
    else
      m_ui.verticalLayout->insertWidget(1, m_ui.hardwareRendererGroup);

    m_hardware_renderer_visible = is_hardware;
  }
  
  if (m_software_renderer_visible != is_software)
  {
    m_ui.softwareRendererGroup->setVisible(is_software);
    if (!is_hardware)
      m_ui.verticalLayout->removeWidget(m_ui.softwareRendererGroup);
    else
      m_ui.verticalLayout->insertWidget(1, m_ui.softwareRendererGroup);

    m_software_renderer_visible = is_software;
  }

  m_ui.useBlitSwapChain->setEnabled(is_dx11);

  // populate adapters
  HostDisplay::AdapterAndModeList modes;
  switch (type)
  {
#ifdef _WIN32
  case GSRendererType::DX11:
    modes = D3D11HostDisplay::StaticGetAdapterAndModeList();
    break;
#endif

  case GSRendererType::OGL:
  case GSRendererType::SW:
  case GSRendererType::Null:
  case GSRendererType::Auto:
  default:
    break;
  }

  // fill+select adapters
  {
    const std::string current_adapter = QtHost::GetBaseStringSettingValue("EmuCore/GS", "Adapter", "");
    QSignalBlocker sb(m_ui.adapter);
    m_ui.adapter->clear();
    m_ui.adapter->setEnabled(!modes.adapter_names.empty());
    m_ui.adapter->addItem(tr("(Default)"));
    for (const std::string& adapter : modes.adapter_names)
    {
      m_ui.adapter->addItem(QString::fromStdString(adapter));
      if (current_adapter == adapter)
        m_ui.adapter->setCurrentIndex(m_ui.adapter->count() - 1);
    }
  }

  // fill+select fullscreen modes. we have to push this to the other panel
  QStringList fs_modes;
  for (const std::string& fs_mode : modes.fullscreen_modes)
    fs_modes.push_back(QString::fromStdString(fs_mode));
  emit fullscreenModesChanged(fs_modes);
}
