#include "PrecompiledHeader.h"

#include "InterfaceSettingsWidget.h"
#include "MainWindow.h"
#include "SettingWidgetBinder.h"
#include "SettingsDialog.h"

static const char* THEME_NAMES[] = {QT_TRANSLATE_NOOP("InterfaceSettingsWidget", "Native"),
                                    QT_TRANSLATE_NOOP("InterfaceSettingsWidget", "Fusion"),
                                    QT_TRANSLATE_NOOP("InterfaceSettingsWidget", "Dark Fusion (Gray)"),
                                    QT_TRANSLATE_NOOP("InterfaceSettingsWidget", "Dark Fusion (Blue)"), nullptr};

static const char* THEME_VALUES[] = {"", "fusion", "darkfusion", "darkfusionblue", nullptr};

InterfaceSettingsWidget::InterfaceSettingsWidget(QWidget* parent, SettingsDialog* dialog) : QWidget(parent)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.inhibitScreensaver, "UI", "InhibitScreensaver", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.discordPresence, "UI", "DiscordPresence", false);
  SettingWidgetBinder::BindWidgetToEnumSetting(m_ui.theme, "UI", "Theme", THEME_NAMES, THEME_VALUES,
                                               MainWindow::DEFAULT_THEME_NAME);
  connect(m_ui.theme, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { emit themeChanged(); });

  dialog->registerWidgetHelp(
    m_ui.inhibitScreensaver, tr("Inhibit Screensaver"), tr("Checked"),
    tr("Prevents the screen saver from activating and the host from sleeping while emulation is running."));

  dialog->registerWidgetHelp(m_ui.discordPresence, tr("Enable Discord Presence"), tr("Unchecked"),
                             tr("Shows the game you are currently playing as part of your profile in Discord."));
  if (true)
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.autoUpdateEnabled, "AutoUpdater", "CheckAtStartup", true);
    dialog->registerWidgetHelp(m_ui.autoUpdateEnabled, tr("Enable Automatic Update Check"), tr("Checked"),
                               tr("Automatically checks for updates to the program on startup. Updates can be deferred "
                                  "until later or skipped entirely."));

    // m_ui.autoUpdateTag->addItems(AutoUpdaterDialog::getTagList());
    // SettingWidgetBinder::BindWidgetToStringSetting(m_ui.autoUpdateTag, "AutoUpdater", "UpdateTag",
    // AutoUpdaterDialog::getDefaultTag());

    // m_ui.autoUpdateCurrentVersion->setText(tr("%1 (%2)").arg(g_scm_tag_str).arg(g_scm_date_str));
    // connect(m_ui.checkForUpdates, &QPushButton::clicked, [this]() {
    // m_host_interface->getMainWindow()->checkForUpdates(true); });
  }
  else
  {
    m_ui.verticalLayout->removeWidget(m_ui.automaticUpdaterGroup);
    m_ui.automaticUpdaterGroup->hide();
  }
}

InterfaceSettingsWidget::~InterfaceSettingsWidget() = default;
