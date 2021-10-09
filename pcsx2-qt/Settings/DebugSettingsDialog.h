#pragma once
#include "ui_DebugSettingsDialog.h"
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <array>

class SystemDebugSettingsWidget;
class GraphicsDebugSettingsWidget;

class DebugSettingsDialog final : public QDialog
{
  Q_OBJECT

public:
  enum class Category
  {
    SystemDebugSettings,
    GraphicsDebugSettings,
    Count
  };

  DebugSettingsDialog(QWidget* parent = nullptr);
  ~DebugSettingsDialog();

  SystemDebugSettingsWidget* getSystemDebugSettingsWidget() const { return m_system_settings; }
  GraphicsDebugSettingsWidget* getGraphicsDebugSettingsWidget() const { return m_graphics_settings; }

  void registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text);
  bool eventFilter(QObject* object, QEvent* event) override;

Q_SIGNALS:
  void settingsResetToDefaults();

public Q_SLOTS:
  void setCategory(Category category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);

private:
  void setCategoryHelpTexts();

  Ui::DebugSettingsDialog m_ui;

  SystemDebugSettingsWidget* m_system_settings = nullptr;
  GraphicsDebugSettingsWidget* m_graphics_settings = nullptr;

  std::array<QString, static_cast<int>(Category::Count)> m_category_help_text;

  QObject* m_current_help_widget = nullptr;
  QMap<QObject*, QString> m_widget_help_text_map;
};
