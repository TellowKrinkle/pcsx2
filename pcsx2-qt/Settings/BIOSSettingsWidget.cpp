#include "PrecompiledHeader.h"

#include <QtGui/QIcon>
#include <QtWidgets/QFileDialog>
#include <algorithm>

#include "pcsx2/ps2/BiosTools.h"

#include "BIOSSettingsWidget.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "SettingsDialog.h"

#if 0

static void populateDropDownForRegion(ConsoleRegion region, QComboBox* cb,
                                      std::vector<std::pair<std::string, const BIOS::ImageInfo*>>& images)
{
  QSignalBlocker sb(cb);
  cb->clear();

  cb->addItem(QIcon(QStringLiteral(":/icons/system-search.png")), qApp->translate("BIOSSettingsWidget", "Auto-Detect"));

  std::sort(images.begin(), images.end(), [region](const auto& left, const auto& right) {
    const bool left_region_match = (left.second && left.second->region == region);
    const bool right_region_match = (right.second && right.second->region == region);
    if (left_region_match && !right_region_match)
      return true;
    else if (right_region_match && !left_region_match)
      return false;

    return left.first < right.first;
  });

  for (const auto& [name, info] : images)
  {
    QIcon icon;
    if (info)
    {
      switch (info->region)
      {
        case ConsoleRegion::NTSC_J:
          icon = QIcon(QStringLiteral(":/icons/flag-jp.png"));
          break;
        case ConsoleRegion::PAL:
          icon = QIcon(QStringLiteral(":/icons/flag-eu.png"));
          break;
        case ConsoleRegion::NTSC_U:
          icon = QIcon(QStringLiteral(":/icons/flag-uc.png"));
          break;
        default:
          icon = QIcon(QStringLiteral(":/icons/applications-other.png"));
          break;
      }
    }
    else
    {
      icon = QIcon(QStringLiteral(":/icons/applications-other.png"));
    }

    QString name_str(QString::fromStdString(name));
    cb->addItem(icon,
                QStringLiteral("%1 (%2)")
                  .arg(info ? QString(info->description) : qApp->translate("BIOSSettingsWidget", "Unknown"))
                  .arg(name_str),
                QVariant(name_str));
  }
}

static void setDropDownValue(QComboBox* cb, const std::string& name)
{
  QSignalBlocker sb(cb);

  if (name.empty())
  {
    cb->setCurrentIndex(0);
    return;
  }

  QString qname(QString::fromStdString(name));
  for (int i = 1; i < cb->count(); i++)
  {
    if (cb->itemData(i) == qname)
    {
      cb->setCurrentIndex(i);
      return;
    }
  }

  cb->addItem(qname, QVariant(qname));
  cb->setCurrentIndex(cb->count() - 1);
}

#endif

BIOSSettingsWidget::BIOSSettingsWidget(QWidget* parent, SettingsDialog* dialog) : QWidget(parent)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_ui.fastBoot, "EmuCore", "EnableFastBoot", true);

  dialog->registerWidgetHelp(m_ui.fastBoot, tr("Fast Boot"), tr("Unchecked"),
                             tr("Patches the BIOS to skip the console's boot animation."));

  updateSearchDirectory();
  refreshList();

  connect(m_ui.searchDirectory, &QLineEdit::textChanged, [this](const QString& text) {
    QtHost::SetBaseStringSettingValue("Folders", "Bios", text.toUtf8().constData());
    QtHost::UpdateFolders();
    refreshList();
  });
  connect(m_ui.resetSearchDirectory, &QPushButton::clicked, [this]() {
    QtHost::RemoveBaseSettingValue("Folders", "Bios");
    QtHost::UpdateFolders();
    updateSearchDirectory();
    refreshList();
  });
  connect(m_ui.browseSearchDirectory, &QPushButton::clicked, this, &BIOSSettingsWidget::browseSearchDirectory);
  connect(m_ui.openSearchDirectory, &QPushButton::clicked, this, &BIOSSettingsWidget::openSearchDirectory);
  connect(m_ui.refresh, &QPushButton::clicked, this, &BIOSSettingsWidget::refreshList);
  connect(m_ui.fileList, &QTreeWidget::currentItemChanged, this, &BIOSSettingsWidget::listItemChanged);
}

BIOSSettingsWidget::~BIOSSettingsWidget()
{
  if (m_refresh_thread)
    m_refresh_thread->wait();
}

void BIOSSettingsWidget::refreshList()
{
  if (m_refresh_thread)
  {
    m_refresh_thread->wait();
    delete m_refresh_thread;
  }

  QSignalBlocker blocker(m_ui.fileList);
  m_ui.fileList->clear();
  m_ui.fileList->setEnabled(false);

  m_refresh_thread = new RefreshThread(this, m_ui.searchDirectory->text());
  m_refresh_thread->start();
}

void BIOSSettingsWidget::browseSearchDirectory()
{
  QString directory = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(
    QtUtils::GetRootWidget(this), tr("Select Directory"), m_ui.searchDirectory->text()));
  if (directory.isEmpty())
    return;

  m_ui.searchDirectory->setText(directory);
}

void BIOSSettingsWidget::openSearchDirectory()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(m_ui.searchDirectory->text()));
}

void BIOSSettingsWidget::updateSearchDirectory()
{
  // this will generate a full path
  m_ui.searchDirectory->setText(QtUtils::WxStringToQString(EmuFolders::Bios.ToString()));
}

void BIOSSettingsWidget::listRefreshed(const QVector<BIOSInfo>& items)
{
  const std::string selected_bios(QtHost::GetBaseStringSettingValue("Filenames", "BIOS"));

  QSignalBlocker sb(m_ui.fileList);
  for (const BIOSInfo& bi : items)
  {
    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setText(0, QString::fromStdString(bi.filename));
    item->setText(1, QString::fromStdString(bi.description));

    switch (bi.region)
    {
      case 2: // Japan
        item->setIcon(0, QIcon(QStringLiteral(":/icons/flag-jp.png")));
        break;

      case 3: // USA
        item->setIcon(0, QIcon(QStringLiteral(":/icons/flag-uc.png")));
        break;

      case 4: // Europe
        item->setIcon(0, QIcon(QStringLiteral(":/icons/flag-eu.png")));
        break;

      case 5: // HK
      case 6: // Free
      case 7: // China
      case 0: // T10K
      case 1: // Test
      default:
        item->setIcon(0, QIcon(QStringLiteral(":/icons/flag-jp.png")));
        break;
    }

    m_ui.fileList->addTopLevelItem(item);

    if (bi.filename == selected_bios)
      item->setSelected(true);
  }
  m_ui.fileList->setEnabled(true);
}

void BIOSSettingsWidget::listItemChanged(const QTreeWidgetItem* current, const QTreeWidgetItem* previous)
{
  QtHost::SetBaseStringSettingValue("Filenames", "BIOS", current->text(0).toUtf8().constData());
}

BIOSSettingsWidget::RefreshThread::RefreshThread(BIOSSettingsWidget* parent, const QString& directory)
  : QThread(parent), m_parent(parent), m_directory(directory)
{
}

BIOSSettingsWidget::RefreshThread::~RefreshThread() = default;

void BIOSSettingsWidget::RefreshThread::run()
{
  QVector<BIOSInfo> items;

  QDir dir(m_directory);
  if (dir.exists())
  {
    for (const QFileInfo& info : dir.entryInfoList(QDir::Files))
    {
      BIOSInfo bi;
      QString full_path(info.absoluteFilePath());
      if (!IsBIOS(full_path.toUtf8().constData(), bi.version, bi.description, bi.region, bi.zone))
        continue;

      bi.filename = info.fileName().toStdString();
      items.push_back(std::move(bi));
    }
  }

  QMetaObject::invokeMethod(m_parent, "listRefreshed", Qt::QueuedConnection, Q_ARG(const QVector<BIOSInfo>&, items));
}
