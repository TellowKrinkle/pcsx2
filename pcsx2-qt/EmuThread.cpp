#include "PrecompiledHeader.h"

#include <QtWidgets/QApplication>

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Exceptions.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Frontend/ImGuiManager.h"
#include "pcsx2/GS.h"
#include "pcsx2/HostDisplay.h"
#include "pcsx2/PAD/Host/PAD.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "pcsx2/Frontend/OpenGLHostDisplay.h"

#ifdef _WIN32
#include "pcsx2/Frontend/D3D11HostDisplay.h"
#endif

#include "DisplayWidget.h"
#include "EmuThread.h"
#include "MainWindow.h"
#include "QtHost.h"

EmuThread* g_emu_thread = nullptr;
WindowInfo g_gs_window_info;

static std::unique_ptr<HostDisplay> s_host_display;

EmuThread::EmuThread(QThread* ui_thread) : QThread(), m_ui_thread(ui_thread) {}

EmuThread::~EmuThread() = default;

bool EmuThread::isOnEmuThread() const
{
  return QThread::currentThread() == this;
}

void EmuThread::start()
{
  pxAssertRel(!g_emu_thread, "Emu thread does not exist");

  g_emu_thread = new EmuThread(QThread::currentThread());
  g_emu_thread->QThread::start();
  g_emu_thread->m_started_semaphore.acquire();
  g_emu_thread->moveToThread(g_emu_thread);
  g_main_window->connectVMThreadSignals(g_emu_thread);
}

void EmuThread::stop()
{
  pxAssertRel(g_emu_thread, "Emu thread exists");
  pxAssertRel(!g_emu_thread->isOnEmuThread(), "Not called on the emu thread");

  QMetaObject::invokeMethod(g_emu_thread, &EmuThread::stopInThread, Qt::QueuedConnection);
  while (g_emu_thread->isRunning())
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
}

void EmuThread::stopInThread()
{
  if (VMManager::HasValidVM())
    destroyVM();

  m_event_loop->quit();
  m_shutdown_flag.store(true);
}

void EmuThread::startVM(std::shared_ptr<VMBootParameters> boot_params)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "startVM", Qt::QueuedConnection,
                              Q_ARG(std::shared_ptr<VMBootParameters>, boot_params));
    return;
  }

  pxAssertRel(!VMManager::HasValidVM(), "VM is shut down");

  emit onVMStarting();

  // create the display, this may take a while...
  m_is_fullscreen = boot_params->fullscreen.value_or(QtHost::GetBaseBoolSettingValue("UI", "StartFullscreen", false));
  m_is_rendering_to_main = !m_is_fullscreen && QtHost::GetBaseBoolSettingValue("UI", "RenderToMainWindow", true);
  if (!VMManager::Initialize(*boot_params))
  {
    emit onVMStopped();
    return;
  }

  emit onVMStarted();
  VMManager::SetState(VMState::Running);
  m_event_loop->quit();
}

void EmuThread::resetVM()
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::resetVM, Qt::QueuedConnection);
    return;
  }

  VMManager::Reset();
}

void EmuThread::pauseVM()
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::pauseVM, Qt::QueuedConnection);
    return;
  }

  if (VMManager::GetState() != VMState::Running)
    return;

  VMManager::SetState(VMState::Paused);
}

void EmuThread::resumeVM()
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::resumeVM, Qt::QueuedConnection);
    return;
  }

  if (VMManager::GetState() != VMState::Paused)
    return;

  PerformanceMetrics::Reset();
  VMManager::SetState(VMState::Running);
  m_event_loop->quit();
  emit onVMResumed();
}

void EmuThread::setVMPaused(bool paused)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "setVMPaused", Qt::QueuedConnection, Q_ARG(bool, paused));
    return;
  }

  paused ? pauseVM() : resumeVM();
}

void EmuThread::shutdownVM(bool allow_save_to_state /* = true */, bool blocking /* = false */)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "shutdownVM", Qt::QueuedConnection, Q_ARG(bool, allow_save_to_state),
                              Q_ARG(bool, blocking));

    if (blocking)
    {
      // we need to yield here, since the display gets destroyed
      while (VMManager::HasValidVM())
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
    }

    return;
  }

  const VMState state = VMManager::GetState();
  if (state == VMState::Paused)
    m_event_loop->quit();
  else if (state != VMState::Running)
    return;

  VMManager::SetState(VMState::Stopping);
}

void EmuThread::loadState(const QString& filename)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  if (!VMManager::HasValidVM())
    return;

  VMManager::LoadState(filename.toUtf8().constData());
}

void EmuThread::loadStateFromSlot(qint32 slot)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "loadStateFromSlot", Qt::QueuedConnection, Q_ARG(qint32, slot));
    return;
  }

  if (!VMManager::HasValidVM())
    return;

  VMManager::LoadStateFromSlot(slot);
}

void EmuThread::saveState(const QString& filename)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "saveState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  if (!VMManager::HasValidVM())
    return;

  if (!VMManager::SaveState(filename.toUtf8().constData()))
  {
    // this one is usually the result of a user-chosen path, so we can display a message box safely here
    Console.Error("Failed to save state");
  }
}

void EmuThread::saveStateToSlot(qint32 slot)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "saveStateToSlot", Qt::QueuedConnection, Q_ARG(qint32, slot));
    return;
  }

  if (!VMManager::HasValidVM())
    return;

  VMManager::SaveStateToSlot(slot);
}

void EmuThread::run()
{
  PerformanceMetrics::SetCPUThreadTimer(Common::ThreadCPUTimer::GetForCallingThread());
  m_event_loop = new QEventLoop();
  m_started_semaphore.release();

  if (!VMManager::InitializeMemory())
    pxFailRel("Failed to allocate memory map");

  while (!m_shutdown_flag.load())
  {
    if (!VMManager::HasValidVM())
    {
      m_event_loop->exec();
      continue;
    }

    executeVM();
  }

  VMManager::ReleaseMemory();
  PerformanceMetrics::SetCPUThreadTimer(Common::ThreadCPUTimer());
  moveToThread(m_ui_thread);
  deleteLater();
}

void EmuThread::destroyVM()
{
  VMManager::Shutdown();

  onDestroyDisplayRequested();
  emit onVMStopped();
}

void EmuThread::executeVM()
{
  for (;;)
  {
    switch (VMManager::GetState())
    {
      case VMState::Starting:
        pxFailRel("Shouldn't be in the starting state state");
        continue;

      case VMState::Paused:
        emit onVMPaused();
        m_event_loop->exec();
        continue;

      case VMState::Running:
        m_event_loop->processEvents(QEventLoop::AllEvents);
        VMManager::Execute();
        continue;

      case VMState::Stopping:
        destroyVM();
        m_event_loop->processEvents(QEventLoop::AllEvents);
        return;
    }
  }
}

void EmuThread::toggleFullscreen()
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::toggleFullscreen, Qt::QueuedConnection);
    return;
  }

  setFullscreen(!m_is_fullscreen);
}

void EmuThread::setFullscreen(bool fullscreen)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "setFullscreen", Qt::QueuedConnection, Q_ARG(bool, fullscreen));
    return;
  }

  if (!VMManager::HasValidVM() || m_is_fullscreen == fullscreen)
    return;

  // This will call back to us on the MTGS thread.
  m_is_fullscreen = fullscreen;
  GetMTGS().UpdateDisplayWindow();
  GetMTGS().WaitGS();
}

void EmuThread::applySettings()
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::applySettings, Qt::QueuedConnection);
    return;
  }

  // If the VM isn't running, no need to apply, since we'll reload at startup anyway.
  if (!VMManager::HasValidVM())
    return;

  checkForSettingChanges();
  VMManager::ApplySettings();
}

void EmuThread::checkForSettingChanges()
{
  const bool render_to_main = QtHost::GetBaseBoolSettingValue("UI", "RenderToMainWindow", true);
  if (!m_is_fullscreen && m_is_rendering_to_main != render_to_main)
  {
    GetMTGS().UpdateDisplayWindow();
    GetMTGS().WaitGS();
  }
}

void EmuThread::toggleSoftwareRendering()
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::toggleSoftwareRendering, Qt::QueuedConnection);
    return;
  }

  if (!VMManager::HasValidVM())
    return;

  GetMTGS().ToggleSoftwareRendering();
}

void EmuThread::switchRenderer(GSRendererType renderer)
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, "switchRenderer", Qt::QueuedConnection, Q_ARG(GSRendererType, renderer));
    return;
  }

  if (!VMManager::HasValidVM())
    return;

  GetMTGS().SwitchRenderer(renderer);
}

void EmuThread::reloadPatches()
{
  if (!isOnEmuThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::reloadPatches, Qt::QueuedConnection);
    return;
  }

  if (!VMManager::HasValidVM())
    return;

  VMManager::ReloadPatches(true);
}

void EmuThread::connectDisplaySignals(DisplayWidget* widget)
{
  widget->disconnect(this);

  connect(widget, &DisplayWidget::windowFocusEvent, this, &EmuThread::onDisplayWindowFocused);
  connect(widget, &DisplayWidget::windowResizedEvent, this, &EmuThread::onDisplayWindowResized);
  // connect(widget, &DisplayWidget::windowRestoredEvent, this, &EmuThread::redrawDisplayWindow);
  connect(widget, &DisplayWidget::windowClosedEvent, []() { g_emu_thread->shutdownVM(true, true); });
  connect(widget, &DisplayWidget::windowKeyEvent, this, &EmuThread::onDisplayWindowKeyEvent);
  connect(widget, &DisplayWidget::windowMouseMoveEvent, this, &EmuThread::onDisplayWindowMouseMoveEvent);
  connect(widget, &DisplayWidget::windowMouseButtonEvent, this, &EmuThread::onDisplayWindowMouseButtonEvent);
  connect(widget, &DisplayWidget::windowMouseWheelEvent, this, &EmuThread::onDisplayWindowMouseWheelEvent);
}

void EmuThread::onDisplayWindowMouseMoveEvent(int x, int y) {}

void EmuThread::onDisplayWindowMouseButtonEvent(int button, bool pressed)
{
  if (button == 2)
    VMManager::SetLimiterMode(pressed ? LimiterModeType::Unlimited : LimiterModeType::Nominal);
}

void EmuThread::onDisplayWindowMouseWheelEvent(const QPoint& delta_angle) {}

void EmuThread::onDisplayWindowKeyEvent(int key, int mods, bool pressed)
{
  HostKeyEvent ev;
  ev.type = pressed ? HostKeyEvent::Type::KeyPressed : HostKeyEvent::Type::KeyReleased;
  ev.key = static_cast<u32>(key);
  PAD::HandleHostInputEvent(ev);
}

void EmuThread::onDisplayWindowResized(int width, int height, float scale)
{
  if (!VMManager::HasValidVM())
    return;

  GetMTGS().ResizeDisplayWindow(width, height, scale);
}

void EmuThread::onDisplayWindowFocused() {}

void EmuThread::updateDisplay()
{
  pxAssertRel(!isOnEmuThread(), "Not on emu thread");

  // finished with the display for now
  HostDisplay* display = Host::GetHostDisplay();
  display->DoneRenderContextCurrent();

  // but we should get it back after this call
  DisplayWidget* widget = onUpdateDisplayRequested(m_is_fullscreen, !m_is_fullscreen && m_is_rendering_to_main);
  if (!widget || !display->MakeRenderContextCurrent())
  {
    pxFailRel("Failed to recreate context after updating");
    return;
  }

  // this is always a new widget, so reconnect it
  connectDisplaySignals(widget);
}

HostDisplay* EmuThread::acquireHostDisplay(HostDisplay::RenderAPI api)
{
  s_host_display = HostDisplay::CreateDisplayForAPI(api);
  if (!s_host_display)
    return nullptr;

  DisplayWidget* widget = emit onCreateDisplayRequested(m_is_fullscreen, m_is_rendering_to_main);
  if (!widget)
  {
    s_host_display.reset();
    return nullptr;
  }

  connectDisplaySignals(widget);

  if (!s_host_display->MakeRenderContextCurrent())
  {
    Console.Error("Failed to make render context current");
    releaseHostDisplay();
    return nullptr;
  }

  if (!s_host_display->InitializeRenderDevice(StringUtil::wxStringToUTF8String(EmuFolders::Cache.ToString()), false) ||
      !ImGuiManager::Initialize())
  {
    Console.Error("Failed to initialize device/imgui");
    releaseHostDisplay();
    return nullptr;
  }

  g_gs_window_info = s_host_display->GetWindowInfo();

  return s_host_display.get();
}

void EmuThread::releaseHostDisplay()
{
  ImGuiManager::Shutdown();

  if (s_host_display)
  {
    s_host_display->DestroyRenderSurface();
    s_host_display->DestroyRenderDevice();
  }

  g_gs_window_info = WindowInfo();

  emit onDestroyDisplayRequested();

  s_host_display.reset();
}

HostDisplay* Host::GetHostDisplay()
{
  return s_host_display.get();
}

HostDisplay* Host::AcquireHostDisplay(HostDisplay::RenderAPI api)
{
  return g_emu_thread->acquireHostDisplay(api);
}

void Host::ReleaseHostDisplay()
{
  g_emu_thread->releaseHostDisplay();
}

void Host::BeginFrame()
{
  // ProcessEvents();
  ImGuiManager::NewFrame();
}

bool Host::BeginPresentFrame(bool frame_skip)
{
  return s_host_display->BeginPresent(frame_skip);
}

void Host::EndPresentFrame()
{
  ImGuiManager::RenderOSD();

  s_host_display->EndPresent();
}

void Host::ResizeHostDisplay(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
  s_host_display->ResizeRenderWindow(new_window_width, new_window_height, new_window_scale);
  ImGuiManager::WindowResized();
}

void Host::UpdateHostDisplay()
{
  g_emu_thread->updateDisplay();
  ImGuiManager::WindowResized();
}

void Host::GameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name,
                       u32 game_crc)
{
  emit g_emu_thread->onGameChanged(QString::fromStdString(disc_path), QString::fromStdString(game_serial),
                                   QString::fromStdString(game_name), game_crc);
}

void Host::PumpMessagesOnCPUThread()
{
  g_emu_thread->getEventLoop()->processEvents(QEventLoop::AllEvents);
}

static alignas(16) SysMtgsThread s_mtgs_thread;

SysMtgsThread& GetMTGS()
{
  return s_mtgs_thread;
}
