#pragma once

#include <QtWidgets/QWidget>

#include "ui_DisplaySettingsWidget.h"

class SettingsDialog;

class DisplaySettingsWidget : public QWidget
{
  Q_OBJECT

public:
  DisplaySettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~DisplaySettingsWidget();

public Q_SLOTS:
  void onFullscreenModesChanged(const QStringList& modes);

private Q_SLOTS:
  void onIntegerScalingChanged();
  void onFullscreenModeChanged(int index);

private:
  Ui::DisplaySettingsWidget m_ui;
};
