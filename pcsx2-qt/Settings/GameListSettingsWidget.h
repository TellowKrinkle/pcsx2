#pragma once
#include <string>
#include <QtWidgets/QWidget>

#include "ui_GameListSettingsWidget.h"

class SettingsDialog;

class GameListSearchDirectoriesModel;

class GameListSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GameListSettingsWidget(QWidget* parent, SettingsDialog* dialog);
  ~GameListSettingsWidget();

  bool addExcludedPath(const std::string& path);
  void refreshExclusionList();

public Q_SLOTS:
  void addSearchDirectory(QWidget* parent_widget);

private Q_SLOTS:
  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onAddExcludedPathButtonClicked();
  void onRemoveExcludedPathButtonClicked();
  void onScanForNewGamesClicked();
  void onRescanAllGamesClicked();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  void addPathToTable(const std::string& path, bool recursive);
  void refreshDirectoryList();
  void addSearchDirectory(const QString& path, bool recursive);
  void removeSearchDirectory(const QString& path);

  Ui::GameListSettingsWidget m_ui;
};
