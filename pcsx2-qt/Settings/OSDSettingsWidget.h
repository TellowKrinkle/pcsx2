#pragma once

#include <QtWidgets/QWidget>

#include "ui_OSDSettingsWidget.h"

class SettingsDialog;

class OSDSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  OSDSettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~OSDSettingsWidget();

private:
  Ui::OSDSettingsWidget m_ui;
};
