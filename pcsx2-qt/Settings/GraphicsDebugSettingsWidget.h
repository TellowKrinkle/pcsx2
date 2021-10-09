#pragma once

#include <QtWidgets/QWidget>

#include "ui_GraphicsDebugSettingsWidget.h"

class DebugSettingsDialog;

class GraphicsDebugSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit GraphicsDebugSettingsWidget(QWidget* parent, DebugSettingsDialog* dialog);
  ~GraphicsDebugSettingsWidget();

private:
  Ui::GraphicsDebugSettingsWidget m_ui;
};
