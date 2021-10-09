#include "PrecompiledHeader.h"

#include <QtWidgets/QMessageBox>
#include <limits>

#include "GraphicsDebugSettingsWidget.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "DebugSettingsDialog.h"

GraphicsDebugSettingsWidget::GraphicsDebugSettingsWidget(QWidget* parent, DebugSettingsDialog* dialog) : QWidget(parent)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToFloatSetting(m_ui.ntscFrameRate, "EmuCore/GS", "FramerateNTSC", 59.94f);
  SettingWidgetBinder::BindWidgetToFloatSetting(m_ui.palFrameRate, "EmuCore/GS", "FrameratePAL", 50.00f);
}

GraphicsDebugSettingsWidget::~GraphicsDebugSettingsWidget() = default;
