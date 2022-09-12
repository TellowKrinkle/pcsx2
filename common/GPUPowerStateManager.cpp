/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "GPUPowerStateManager.h"

#include <optional>

#ifdef __APPLE__
#include <IOKit/IOKitLib.h>
#endif

GPUPowerStateManager::Setter::~Setter() = default;

#ifdef __APPLE__
class MacIntelGPUPowerStateSetter : public GPUPowerStateManager::Setter
{
	// Intel GPU Power State Manager based off decompilation of Apple's MTLReplayer GTPMService.xpc
	// Fun private API things, yay
	bool high_power_forced = false;
	io_connect_t service = 0;

	enum AGPMType {
		AGPMTypeIG = 2,
	};

	enum AGPMSelector {
		AGPMSelectorBeginCommands    = 0x1c85,
		AGPMSelectorEndCommands      = 0x1c86,
		AGPMSelectorGetMaxPowerState = 0x1c88,
		AGPMSelectorSetPowerState    = 0x1c89,
		AGPMSelectorGetControlState  = 0x1c8a,
		AGPMSelectorSetControlState  = 0x1c8b,
		AGPMSelectorGetType          = 0x1c91,
	};

	static io_connect_t FindService(const char* name, uint32_t type, bool (*match)(io_connect_t))
	{
		CFMutableDictionaryRef dic = IOServiceMatching(name);
		io_iterator_t iter;
		if (IOServiceGetMatchingServices(0, dic, &iter) != kIOReturnSuccess)
			return 0;
		io_connect_t output = 0;
		while (io_object_t obj = IOIteratorNext(iter)) {
			io_connect_t con;
			if (IOServiceOpen(obj, mach_task_self(), type, &con) == kIOReturnSuccess) {
				if (!match || match(con))
					output = con;
				else
					IOServiceClose(con);
			}
			IOObjectRelease(obj);
			if (output)
				break;
		}
		IOObjectRelease(iter);
		return output;
	}

	static std::optional<uint64_t> CallGetter(io_connect_t service, AGPMSelector method)
	{
		uint64_t value;
		uint32_t cnt = 1;
		if (IOConnectCallScalarMethod(service, method, nullptr, 0, &value, &cnt) != kIOReturnSuccess || cnt != 1)
			return std::nullopt;
		return value;
	}

	static bool IsIGService(io_connect_t service)
	{
		auto res = CallGetter(service, AGPMSelectorGetType);
		return res.has_value() && *res == AGPMTypeIG;
	}

	bool BeginCommands()
	{
		return IOConnectCallScalarMethod(service, AGPMSelectorBeginCommands, nullptr, 0, nullptr, nullptr) == kIOReturnSuccess;
	}

	bool EndCommands()
	{
		return IOConnectCallScalarMethod(service, AGPMSelectorEndCommands, nullptr, 0, nullptr, nullptr) == kIOReturnSuccess;
	}

	bool SetControlState(bool forced)
	{
		uint64_t input = forced;
		uint64_t output;
		uint32_t cnt = 1;
		return IOConnectCallScalarMethod(service, AGPMSelectorSetControlState, &input, 1, &output, &cnt) == kIOReturnSuccess && cnt == 1;
	}

	bool SetPowerState(uint32_t state)
	{
		uint64_t input = state;
		return IOConnectCallScalarMethod(service, AGPMSelectorSetPowerState, &input, 1, nullptr, nullptr) == kIOReturnSuccess;
	}

public:
	MacIntelGPUPowerStateSetter()
	{
		service = FindService("AGPM", 0, IsIGService);
		if (!service)
			return;
		if (auto control_state = CallGetter(service, AGPMSelectorGetControlState))
			high_power_forced = *control_state;
		SetForceHighPowerState(false);
	}

	~MacIntelGPUPowerStateSetter() override
	{
		SetForceHighPowerState(false);
		if (service)
			IOServiceClose(service);
	}

	void SetForceHighPowerState(bool forced) override
	{
		if (!service || high_power_forced == forced)
			return;
		if (!BeginCommands())
			return;
		if (forced)
		{
			if (SetControlState(true))
			{
				// 0 is highest power, higher values are lower power
				// You can get the lowest power value with AGPMSelectorGetMaxPowerState, but we don't really care
				if (SetPowerState(0))
					high_power_forced = true;
			}
		}
		else
		{
			if (SetControlState(false))
				high_power_forced = false;
		}
		EndCommands();
	}
};
#endif

GPUPowerStateManager::Setter* GPUPowerStateManager::Setter::CreateForGPU(std::string_view name)
{
	bool enabled = true;
	if (const char* env = getenv("FORCE_HIGH_GPU_POWER"))
		enabled = env[0] == 'y' || env[0] == 'Y' || env[0] == '1';
	if (!enabled)
		return nullptr;
#ifdef __APPLE__
	if (name.find("Intel") != name.npos)
		return new MacIntelGPUPowerStateSetter();
#endif
	return nullptr;
}

GPUPowerStateManager GPUPowerStateManager::shared;
