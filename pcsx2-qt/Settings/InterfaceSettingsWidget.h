#pragma once

#include <QtWidgets/QWidget>

#include "ui_InterfaceSettingsWidget.h"

class SettingsDialog;

class InterfaceSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit InterfaceSettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~InterfaceSettingsWidget();

Q_SIGNALS:
  void themeChanged();

private:
  Ui::InterfaceSettingsWidget m_ui;
};
