#include "PrecompiledHeader.h"

#include "QtHost.h"
#include "DebugSettingsDialog.h"

#include "SystemDebugSettingsWidget.h"
#include "GraphicsDebugSettingsWidget.h"

#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTextEdit>

static constexpr char DEFAULT_SETTING_HELP_TEXT[] = "";

DebugSettingsDialog::DebugSettingsDialog(QWidget* parent /* = nullptr */) : QDialog(parent)
{
  m_ui.setupUi(this);
  setCategoryHelpTexts();

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_system_settings = new SystemDebugSettingsWidget(m_ui.settingsContainer, this);
  m_graphics_settings = new GraphicsDebugSettingsWidget(m_ui.settingsContainer, this);

  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::SystemDebugSettings), m_system_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GraphicsDebugSettings), m_graphics_settings);

  m_ui.settingsCategory->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  m_ui.settingsCategory->setCurrentRow(0);
  m_ui.settingsContainer->setCurrentIndex(0);
  m_ui.helpText->setText(m_category_help_text[0]);
  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this, &DebugSettingsDialog::onCategoryCurrentRowChanged);
  connect(m_ui.closeButton, &QPushButton::clicked, this, &DebugSettingsDialog::accept);
}

DebugSettingsDialog::~DebugSettingsDialog() = default;

void DebugSettingsDialog::setCategoryHelpTexts()
{
  m_category_help_text[static_cast<int>(Category::SystemDebugSettings)] =
    tr("<strong>System Settings</strong><hr>TODO.<br><br>Mouse "
       "over "
       "an option for additional information.");
}

void DebugSettingsDialog::setCategory(Category category)
{
  if (category >= Category::Count)
    return;

  m_ui.settingsCategory->setCurrentRow(static_cast<int>(category));
}

void DebugSettingsDialog::onCategoryCurrentRowChanged(int row)
{
  Q_ASSERT(row < static_cast<int>(Category::Count));
  m_ui.settingsContainer->setCurrentIndex(row);
  m_ui.helpText->setText(m_category_help_text[row]);
}

void DebugSettingsDialog::registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text)
{
  // construct rich text with formatted description
  QString full_text;
  full_text += "<table width='100%' cellpadding='0' cellspacing='0'><tr><td><strong>";
  full_text += title;
  full_text += "</strong></td><td align='right'><strong>";
  full_text += tr("Recommended Value");
  full_text += ": </strong>";
  full_text += recommended_value;
  full_text += "</td></table><hr>";
  full_text += text;

  m_widget_help_text_map[object] = std::move(full_text);
  object->installEventFilter(this);
}

bool DebugSettingsDialog::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Enter)
  {
    auto iter = m_widget_help_text_map.constFind(object);
    if (iter != m_widget_help_text_map.end())
    {
      m_current_help_widget = object;
      m_ui.helpText->setText(iter.value());
    }
  }
  else if (event->type() == QEvent::Leave)
  {
    if (m_current_help_widget)
    {
      m_current_help_widget = nullptr;
      m_ui.helpText->setText(m_category_help_text[m_ui.settingsCategory->currentRow()]);
    }
  }

  return QDialog::eventFilter(object, event);
}