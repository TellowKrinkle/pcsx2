#include "PrecompiledHeader.h"

#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "SettingsDialog.h"
#include "OSDSettingsWidget.h"
#include <QtWidgets/QMessageBox>

OSDSettingsWidget::OSDSettingsWidget(QWidget* parent, SettingsDialog* dialog) : QWidget(parent)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToFloatSetting(m_ui.osdScale, "EmuCore/GS", "OsdScale", 100.0f);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.osdShowMessages, "EmuCore/GS", "OsdShowMessages", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.osdShowSpeed, "EmuCore/GS", "OsdShowSpeed", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.osdShowFPS, "EmuCore/GS", "OsdShowFPS", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.osdShowCPU, "EmuCore/GS", "OsdShowCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.osdShowResolution, "EmuCore/GS", "OsdShowResolution", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.osdShowGSStats, "EmuCore/GS", "OsdShowGSStats", false);

  dialog->registerWidgetHelp(m_ui.osdShowMessages, tr("Show OSD Messages"), tr("Checked"),
                             tr("Shows on-screen-display messages when events occur such as save states being "
                                "created/loaded, screenshots being taken, etc."));
  dialog->registerWidgetHelp(m_ui.osdShowFPS, tr("Show Game Frame Rate"), tr("Unchecked"),
                             tr("Shows the internal frame rate of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.osdShowSpeed, tr("Show Emulation Speed"), tr("Unchecked"),
    tr("Shows the current emulation speed of the system in the top-right corner of the display as a percentage."));
  dialog->registerWidgetHelp(m_ui.osdShowResolution, tr("Show Resolution"), tr("Unchecked"),
                             tr("Shows the resolution of the game in the top-right corner of the display."));
}

OSDSettingsWidget::~OSDSettingsWidget() = default;

