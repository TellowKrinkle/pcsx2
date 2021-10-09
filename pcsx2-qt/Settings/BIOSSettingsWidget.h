#pragma once
#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtCore/QPair>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>
#include <string>

#include "ui_BIOSSettingsWidget.h"

class SettingsDialog;
class QThread;

// TODO: Move to core.
struct BIOSInfo
{
  std::string filename;
  std::string description;
  std::string zone;
  u32 version;
  u32 region;
};
Q_DECLARE_METATYPE(BIOSInfo);

class BIOSSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit BIOSSettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~BIOSSettingsWidget();

private Q_SLOTS:
  void refreshList();
  void browseSearchDirectory();
  void openSearchDirectory();
  void updateSearchDirectory();

  void listItemChanged(const QTreeWidgetItem* current, const QTreeWidgetItem* previous);
  void listRefreshed(const QVector<BIOSInfo>& items);

private:
  Ui::BIOSSettingsWidget m_ui;

  class RefreshThread final : public QThread
  {
  public:
    RefreshThread(BIOSSettingsWidget* parent, const QString& directory);
    ~RefreshThread();

  protected:
    void run() override;

  private:
    BIOSSettingsWidget* m_parent;
    QString m_directory;
  };

  RefreshThread* m_refresh_thread = nullptr;
};
