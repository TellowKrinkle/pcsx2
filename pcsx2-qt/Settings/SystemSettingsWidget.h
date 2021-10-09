#pragma once

#include <QtWidgets/QWidget>

#include "ui_SystemSettingsWidget.h"

class SettingsDialog;

class SystemSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit SystemSettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~SystemSettingsWidget();

private Q_SLOTS:
  void updateVU1InstantState();

private:
  static int getClampingModeIndex(bool vu);
  static void setClampingMode(bool vu, int index);

  Ui::SystemSettingsWidget m_ui;
};
