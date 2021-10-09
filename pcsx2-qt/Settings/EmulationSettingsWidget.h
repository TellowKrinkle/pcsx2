#pragma once

#include <QtWidgets/QWidget>

#include "ui_EmulationSettingsWidget.h"

class SettingsDialog;

class EmulationSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit EmulationSettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~EmulationSettingsWidget();

private Q_SLOTS:
  void onNormalSpeedIndexChanged(int index);
  void onFastForwardSpeedIndexChanged(int index);
  void onSlowMotionSpeedIndexChanged(int index);
  void onOptimalFramePacingChanged(bool checked);

private:
  void updateOptimalFramePacing();

  Ui::EmulationSettingsWidget m_ui;
};
