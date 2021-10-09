#pragma once

#include <QtWidgets/QWidget>

#include "ui_SystemDebugSettingsWidget.h"

class DebugSettingsDialog;

class SystemDebugSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit SystemDebugSettingsWidget(QWidget* parent, DebugSettingsDialog* dialog);
  ~SystemDebugSettingsWidget();

private:
  Ui::SystemDebugSettingsWidget m_ui;
};
