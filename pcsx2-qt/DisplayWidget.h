#pragma once
#include "common/WindowInfo.h"
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QWidget>
#include <optional>

class DisplayWidget final : public QWidget
{
  Q_OBJECT

public:
  DisplayWidget(QWidget* parent);
  ~DisplayWidget();

  QPaintEngine* paintEngine() const override;

  int scaledWindowWidth() const;
  int scaledWindowHeight() const;
  qreal devicePixelRatioFromScreen() const;

  std::optional<WindowInfo> getWindowInfo() const;

  void setRelativeMode(bool enabled);

Q_SIGNALS:
  void windowFocusEvent();
  void windowResizedEvent(int width, int height, float scale);
  void windowRestoredEvent();
  void windowClosedEvent();
  void windowKeyEvent(int key_code, int mods, bool pressed);
  void windowMouseMoveEvent(int x, int y);
  void windowMouseButtonEvent(int button, bool pressed);
  void windowMouseWheelEvent(const QPoint& angle_delta);

protected:
  bool event(QEvent* event) override;

private:
  QPoint m_relative_mouse_start_position{};
  QPoint m_relative_mouse_last_position{};
  bool m_relative_mouse_enabled = false;
};

class DisplayContainer final : public QStackedWidget
{
  Q_OBJECT

public:
  DisplayContainer();
  ~DisplayContainer();

  static bool IsNeeded(bool fullscreen, bool render_to_main);

  void setDisplayWidget(DisplayWidget* widget);
  DisplayWidget* removeDisplayWidget();

protected:
  bool event(QEvent* event) override;

private:
  DisplayWidget* m_display_widget = nullptr;
};
