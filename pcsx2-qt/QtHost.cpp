#include "PrecompiledHeader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/Frontend/GameList.h"
#include "pcsx2/Frontend/INISettingsInterface.h"
#include "pcsx2/HostSettings.h"
#include "pcsx2/PAD/Host/PAD.h"

#include <QtCore/QTimer>
#include <QtWidgets/QMessageBox>

#include "EmuThread.h"
#include "GameListWidget.h"
#include "MainWindow.h"
#include "QtHost.h"

static constexpr u32 SETTINGS_VERSION = 1;
static constexpr u32 SETTINGS_SAVE_DELAY = 1000;

//////////////////////////////////////////////////////////////////////////
// Local function declarations
//////////////////////////////////////////////////////////////////////////
static void InitializeWxRubbish();
static bool InitializeConfig();
static void SetDefaultConfig();
static void QueueSettingsSave();
static void SaveSettings();

//////////////////////////////////////////////////////////////////////////
// Local variable declarations
//////////////////////////////////////////////////////////////////////////
static std::unique_ptr<QTimer> s_settings_save_timer;
static std::unique_ptr<INISettingsInterface> s_base_settings_interface;

//////////////////////////////////////////////////////////////////////////
// Initialization/Shutdown
//////////////////////////////////////////////////////////////////////////

bool QtHost::Initialize()
{
  qRegisterMetaType<std::shared_ptr<VMBootParameters>>();
  qRegisterMetaType<GSRendererType>();
  qRegisterMetaType<const GameList::Entry*>();

  InitializeWxRubbish();
  if (!InitializeConfig())
  {
    Console.WriteLn("Failed to initialize config.");
    return false;
  }

  return true;
}

void QtHost::Shutdown() {}

static bool SetCriticalFolders()
{
  std::string program_path(FileSystem::GetProgramPath());
  EmuFolders::AppRoot = wxDirName(wxFileName(StringUtil::UTF8StringToWxString(program_path)));
  EmuFolders::DataRoot = EmuFolders::AppRoot;
#ifndef _WIN32
  const char* homedir = getenv("HOME");
  if (homedir)
    EmuFolders::DataRoot = Path::Combine(wxString::FromUTF8(homedir), wxString(L"PCSX2"));
#endif

  EmuFolders::Settings = EmuFolders::DataRoot.Combine(wxDirName(L"inis"));
  EmuFolders::Resources = EmuFolders::AppRoot.Combine(wxDirName(L"resources"));

  // the resources directory should exist, bail out if not
  if (!EmuFolders::Resources.Exists())
  {
    QMessageBox::critical(nullptr, QStringLiteral("Error"),
                          QStringLiteral("Resources directory is missing, your installation is incomplete."));
    return false;
  }

  return true;
}

void QtHost::UpdateFolders()
{
  // TODO: This should happen with the VM thread paused.
  auto lock = Host::GetSettingsLock();
  EmuFolders::LoadConfig(*s_base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();
}

void QtHost::UpdateLogging()
{
  // TODO: Make this an actual option.
  DevConWriterEnabled = true;
}

bool InitializeConfig()
{
  if (!SetCriticalFolders())
    return false;

  const std::string path(Path::CombineStdString(EmuFolders::Settings, "PCSX2.ini"));
  s_base_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
  Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

  uint settings_version;
  if (!s_base_settings_interface->Load() ||
      !s_base_settings_interface->GetUIntValue("UI", "SettingsVersion", &settings_version) ||
      settings_version != SETTINGS_VERSION)
  {
    QMessageBox::critical(
      g_main_window, qApp->translate("QtHost", "Settings Reset"),
      qApp->translate("QtHost", "Settings do not exist or are the incorrect version, resetting to defaults."));
    SetDefaultConfig();
    s_base_settings_interface->Save();
  }

  // TODO: Handle reset to defaults if load fails.
  EmuFolders::LoadConfig(*s_base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();
  QtHost::UpdateLogging();
  return true;
}

void SetDefaultConfig()
{
  EmuConfig = Pcsx2Config();
  EmuFolders::SetDefaults();

  SettingsInterface& si = *s_base_settings_interface.get();
  si.SetUIntValue("UI", "SettingsVersion", SETTINGS_VERSION);

  {
    SettingsSaveWrapper wrapper(si);
    EmuConfig.LoadSave(wrapper);
  }

  EmuFolders::Save(si);
  PAD::SetDefaultConfig(si);
}

std::string QtHost::GetBaseStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  auto lock = Host::GetSettingsLock();
  return s_base_settings_interface->GetStringValue(section, key, default_value);
}

bool QtHost::GetBaseBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
  auto lock = Host::GetSettingsLock();
  return s_base_settings_interface->GetBoolValue(section, key, default_value);
}

int QtHost::GetBaseIntSettingValue(const char* section, const char* key, int default_value /*= 0*/)
{
  auto lock = Host::GetSettingsLock();
  return s_base_settings_interface->GetIntValue(section, key, default_value);
}

float QtHost::GetBaseFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  auto lock = Host::GetSettingsLock();
  return s_base_settings_interface->GetFloatValue(section, key, default_value);
}

std::vector<std::string> QtHost::GetBaseStringListSetting(const char* section, const char* key)
{
  auto lock = Host::GetSettingsLock();
  return s_base_settings_interface->GetStringList(section, key);
}

void QtHost::SetBaseBoolSettingValue(const char* section, const char* key, bool value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetBoolValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseIntSettingValue(const char* section, const char* key, int value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetIntValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseFloatSettingValue(const char* section, const char* key, float value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetFloatValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseStringSettingValue(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetStringValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetStringList(section, key, values);
  QueueSettingsSave();
}

bool QtHost::AddBaseValueToStringList(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!s_base_settings_interface->AddToStringList(section, key, value))
    return false;

  QueueSettingsSave();
  return true;
}

bool QtHost::RemoveBaseValueFromStringList(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!s_base_settings_interface->RemoveFromStringList(section, key, value))
    return false;

  QueueSettingsSave();
  return true;
}

void QtHost::RemoveBaseSettingValue(const char* section, const char* key)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->DeleteValue(section, key);
  QueueSettingsSave();
}

void SaveSettings()
{
  pxAssertRel(!g_emu_thread->isOnEmuThread(), "Saving should happen on the UI thread.");

  {
    auto lock = Host::GetSettingsLock();
    if (!s_base_settings_interface->Save())
      Console.Error("Failed to save settings.");
  }

  s_settings_save_timer->deleteLater();
  s_settings_save_timer.release();
}

void QueueSettingsSave()
{
  if (s_settings_save_timer)
    return;

  s_settings_save_timer = std::make_unique<QTimer>();
  s_settings_save_timer->connect(s_settings_save_timer.get(), &QTimer::timeout, SaveSettings);
  s_settings_save_timer->setSingleShot(true);
  s_settings_save_timer->start(SETTINGS_SAVE_DELAY);
}

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
  const std::string path(Path::CombineStdString(EmuFolders::Resources, filename));
  std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
  if (!ret.has_value())
    Console.Error("Failed to read resource file '%s'", filename);
  return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
  const std::string path(Path::CombineStdString(EmuFolders::Resources, filename));
  std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
  if (!ret.has_value())
    Console.Error("Failed to read resource file to string '%s'", filename);
  return ret;
}

//////////////////////////////////////////////////////////////////////////
// Interface Stuff
//////////////////////////////////////////////////////////////////////////

void OSDlog(ConsoleColors color, bool console, const std::string& str)
{
  Host::AddOSDMessage(str, 10.0f);

  if (console)
    Console.WriteLn(color, str.c_str());
}

void OSDmonitor(ConsoleColors color, const std::string key, const std::string value) {}

const IConsoleWriter* PatchesCon = &Console;

void LoadAllPatchesAndStuff(const Pcsx2Config& cfg)
{
  // FIXME
}

void PatchesVerboseReset()
{
  // FIXME
}

void StateCopy_SaveToSlot(uint num)
{
  pxFailRel("Not implemented");
}

void StateCopy_LoadFromSlot(uint slot, bool isFromBackup)
{
  pxFailRel("Not implemented");
}

#include <wx/module.h>

#ifdef _WIN32
extern "C" HINSTANCE wxGetInstance();
extern void wxSetInstance(HINSTANCE hInst);
#endif

void InitializeWxRubbish()
{
  wxLog::DoCreateOnDemand();
  wxLog::GetActiveTarget();

#ifdef _WIN32
  if (!wxGetInstance())
    wxSetInstance(::GetModuleHandle(NULL));
#endif // _WIN32

  wxModule::RegisterModules();
  wxModule::InitializeModules();
}
