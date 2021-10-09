#pragma once
#include "pcsx2/Host.h"
#include "pcsx2/HostDisplay.h"
#include "pcsx2/HostSettings.h"
#include "pcsx2/VMManager.h"
#include <QtCore/QMetaType>

class SettingsInterface;

class EmuThread;

Q_DECLARE_METATYPE(GSRendererType);
Q_DECLARE_METATYPE(std::shared_ptr<VMBootParameters>);

namespace QtHost
{
bool Initialize();
void Shutdown();

void UpdateFolders();
void UpdateLogging();

/// Thread-safe settings access.
std::string GetBaseStringSettingValue(const char* section, const char* key, const char* default_value = "");
bool GetBaseBoolSettingValue(const char* section, const char* key, bool default_value = false);
int GetBaseIntSettingValue(const char* section, const char* key, int default_value = 0);
float GetBaseFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
std::vector<std::string> GetBaseStringListSetting(const char* section, const char* key);
void SetBaseBoolSettingValue(const char* section, const char* key, bool value);
void SetBaseIntSettingValue(const char* section, const char* key, int value);
void SetBaseFloatSettingValue(const char* section, const char* key, float value);
void SetBaseStringSettingValue(const char* section, const char* key, const char* value);
void SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values);
bool AddBaseValueToStringList(const char* section, const char* key, const char* value);
bool RemoveBaseValueFromStringList(const char* section, const char* key, const char* value);
void RemoveBaseSettingValue(const char* section, const char* key);
}
