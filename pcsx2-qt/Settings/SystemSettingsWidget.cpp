#include "PrecompiledHeader.h"

#include <QtWidgets/QMessageBox>
#include <algorithm>

#include "EmuThread.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "SettingsDialog.h"
#include "SystemSettingsWidget.h"

static constexpr int MINIMUM_EE_CYCLE_RATE = -3;
static constexpr int MAXIMUM_EE_CYCLE_RATE = 3;
static constexpr int DEFAULT_EE_CYCLE_RATE = 0;
static constexpr int DEFAULT_EE_CYCLE_SKIP = 0;

SystemSettingsWidget::SystemSettingsWidget(QWidget* parent, SettingsDialog* dialog) : QWidget(parent)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToIntSetting(m_ui.eeRoundingMode, "EmuCore/CPU", "FPU.Roundmode", 3);
  m_ui.eeClampMode->setCurrentIndex(getClampingModeIndex(false));
  connect(m_ui.eeClampMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { setClampingMode(false, index); });
  m_ui.eeCycleRate->setCurrentIndex(
    std::clamp(QtHost::GetBaseIntSettingValue("EmuCore/Speedhacks", "EECycleRate", DEFAULT_EE_CYCLE_RATE),
               MINIMUM_EE_CYCLE_RATE, MAXIMUM_EE_CYCLE_RATE) +
    (0 - MINIMUM_EE_CYCLE_RATE));
  connect(m_ui.eeCycleRate, QOverload<int>::of(&QComboBox::currentIndexChanged), [](int index) {
    QtHost::SetBaseIntSettingValue("EmuCore/Speedhacks", "EECycleRate", MINIMUM_EE_CYCLE_RATE + index);
    g_emu_thread->applySettings();
  });
  SettingWidgetBinder::BindWidgetToIntSetting(m_ui.eeCycleSkipping, "EmuCore/Speedhacks", "EECycleSkip", 0);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.MTVU, "EmuCore/Speedhacks", "vuThread", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.instantVU1, "EmuCore/Speedhacks", "vu1Instant", true);
  SettingWidgetBinder::BindWidgetToIntSetting(m_ui.vuRoundingMode, "EmuCore/CPU", "VU.Roundmode", 3);
  m_ui.vuClampMode->setCurrentIndex(getClampingModeIndex(true));
  connect(m_ui.vuClampMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { setClampingMode(true, index); });

  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.fastCDVD, "EmuCore/Speedhacks", "fastCDVD", false);

  updateVU1InstantState();
  connect(m_ui.MTVU, &QCheckBox::stateChanged, this, &SystemSettingsWidget::updateVU1InstantState);
}

SystemSettingsWidget::~SystemSettingsWidget() = default;

void SystemSettingsWidget::updateVU1InstantState()
{
  m_ui.instantVU1->setEnabled(!m_ui.MTVU->isChecked());
}

int SystemSettingsWidget::getClampingModeIndex(bool vu)
{
  if (QtHost::GetBaseBoolSettingValue("EmuCore/CPU/Recompiler", vu ? "vuSignOverflow" : "fpuFullMode", false))
    return 3;
  if (QtHost::GetBaseBoolSettingValue("EmuCore/CPU/Recompiler", vu ? "vuExtraOverflow" : "fpuExtraOverflow", false))
    return 2;
  if (QtHost::GetBaseBoolSettingValue("EmuCore/CPU/Recompiler", vu ? "vuOverflow" : "fpuOverflow", true))
    return 1;
  return 0;
}

void SystemSettingsWidget::setClampingMode(bool vu, int index)
{
  QtHost::SetBaseBoolSettingValue("EmuCore/CPU/Recompiler", vu ? "vuSignOverflow" : "fpuFullMode", (index >= 3));
  QtHost::SetBaseBoolSettingValue("EmuCore/CPU/Recompiler", vu ? "vuExtraOverflow" : "fpuExtraOverflow", (index >= 2));
  QtHost::SetBaseBoolSettingValue("EmuCore/CPU/Recompiler", vu ? "vuOverflow" : "fpuOverflow", (index >= 1));
  g_emu_thread->applySettings();
}
