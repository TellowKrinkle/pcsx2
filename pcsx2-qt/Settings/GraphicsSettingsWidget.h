#pragma once

#include <QtWidgets/QWidget>

#include "ui_GraphicsSettingsWidget.h"

class SettingsDialog;

class GraphicsSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GraphicsSettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~GraphicsSettingsWidget();

  void updateRendererDependentOptions();

Q_SIGNALS:
  void fullscreenModesChanged(const QStringList& modes);

private Q_SLOTS:
  void onRendererChanged(int index);
  void onAdapterChanged(int index);

private:
  Ui::GraphicsSettingsWidget m_ui;

  bool m_hardware_renderer_visible = true;
  bool m_software_renderer_visible = true;
};
