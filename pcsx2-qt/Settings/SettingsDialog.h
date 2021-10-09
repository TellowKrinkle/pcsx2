#pragma once
#include "ui_SettingsDialog.h"
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <array>

class InterfaceSettingsWidget;
class GameListSettingsWidget;
class EmulationSettingsWidget;
class BIOSSettingsWidget;
class SystemSettingsWidget;
class DisplaySettingsWidget;
class OSDSettingsWidget;
class GraphicsSettingsWidget;
class AudioSettingsWidget;
class MemoryCardSettingsWidget;
class HotkeySettingsWidget;

class SettingsDialog final : public QDialog
{
  Q_OBJECT

public:
  enum class Category
  {
    InterfaceSettings,
    GameListSettings,
    BIOSSettings,
    EmulationSettings,
    SystemSettings,
    DisplaySettings,
    OSDSettings,
    GraphicsSettings,
    AudioSettings,
    MemoryCardSettings,
    HotkeySettings,
    Count
  };

  SettingsDialog(QWidget* parent = nullptr);
  ~SettingsDialog();

  InterfaceSettingsWidget* getInterfaceSettingsWidget() const { return m_interface_settings; }
  GameListSettingsWidget* getGameListSettingsWidget() const { return m_game_list_settings; }
  BIOSSettingsWidget* getBIOSSettingsWidget() const { return m_bios_settings; }
  EmulationSettingsWidget* getEmulationSettingsWidget() const { return m_emulation_settings; }
  SystemSettingsWidget* getSystemSettingsWidget() const { return m_system_settings; }
  OSDSettingsWidget* getOSDSettingsWidget() const { return m_osd_settings; }
  DisplaySettingsWidget* getDisplaySettingsWidget() const { return m_display_settings; }
  GraphicsSettingsWidget* getGraphicsSettingsWidget() const { return m_graphics_settings; }
  AudioSettingsWidget* getAudioSettingsWidget() const { return m_audio_settings; }
  MemoryCardSettingsWidget* getMemoryCardSettingsWidget() const { return m_memory_card_settings; }
  HotkeySettingsWidget* getHotkeySettingsWidget() const { return m_hotkey_settings; }

  void registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text);
  bool eventFilter(QObject* object, QEvent* event) override;

Q_SIGNALS:
  void settingsResetToDefaults();

public Q_SLOTS:
  void setCategory(Category category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);
  void onRestoreDefaultsClicked();

private:
  void setCategoryHelpTexts();

  Ui::SettingsDialog m_ui;

  InterfaceSettingsWidget* m_interface_settings = nullptr;
  GameListSettingsWidget* m_game_list_settings = nullptr;
  BIOSSettingsWidget* m_bios_settings = nullptr;
  EmulationSettingsWidget* m_emulation_settings = nullptr;
  SystemSettingsWidget* m_system_settings = nullptr;
  DisplaySettingsWidget* m_display_settings = nullptr;
  OSDSettingsWidget* m_osd_settings = nullptr;
  GraphicsSettingsWidget* m_graphics_settings = nullptr;
  AudioSettingsWidget* m_audio_settings = nullptr;
  MemoryCardSettingsWidget* m_memory_card_settings = nullptr;
  HotkeySettingsWidget* m_hotkey_settings = nullptr;

  std::array<QString, static_cast<int>(Category::Count)> m_category_help_text;

  QObject* m_current_help_widget = nullptr;
  QMap<QObject*, QString> m_widget_help_text_map;
};
