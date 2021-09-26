/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#include "PrecompiledHeader.h"

#include "common/StringUtil.h"
#include "common/SettingsInterface.h"

#include "HostSettings.h"

#include "PAD/Host/Global.h"
#include "PAD/Host/Device.h"
#include "PAD/Host/PAD.h"
#include "PAD/Host/KeyStatus.h"
#include "PAD/Host/StateManagement.h"
#include "PAD/Host/Device.h"
#include "PAD/Host/InputManager.h"
#include "PAD/Host/Config.h"

#ifdef SDL_BUILD
#include <SDL.h>
#endif

const u32 revision = 3;
const u32 build = 0; // increase that with each version
#define PAD_SAVE_STATE_VERSION ((revision << 8) | (build << 0))

PADconf g_conf;
KeyStatus g_key_status;

s32 PADinit()
{
	Pad::reset_all();

	query.reset();

	for (int port = 0; port < 2; port++)
		slots[port] = 0;

	return 0;
}

void PADshutdown()
{
}

s32 PADopen(const WindowInfo& wi)
{
	g_key_status.Init();
	EnumerateDevices();
	return 0;
}

void PADclose()
{
	device_manager.devices.clear();
}

s32 PADsetSlot(u8 port, u8 slot)
{
	port--;
	slot--;
	if (port > 1 || slot > 3)
	{
		return 0;
	}
	// Even if no pad there, record the slot, as it is the active slot regardless.
	slots[port] = slot;

	return 1;
}

s32 PADfreeze(FreezeAction mode, freezeData* data)
{
	if (!data)
		return -1;

	if (mode == FreezeAction::Size)
	{
		data->size = sizeof(PadFullFreezeData);
	}
	else if (mode == FreezeAction::Load)
	{
		PadFullFreezeData* pdata = (PadFullFreezeData*)(data->data);

		Pad::stop_vibrate_all();

		if (data->size != sizeof(PadFullFreezeData) || pdata->version != PAD_SAVE_STATE_VERSION ||
			strncmp(pdata->format, "LinPad", sizeof(pdata->format)))
			return 0;

		query = pdata->query;
		if (pdata->query.slot < 4)
		{
			query = pdata->query;
		}

		// Tales of the Abyss - pad fix
		// - restore data for both ports
		for (int port = 0; port < 2; port++)
		{
			for (int slot = 0; slot < 4; slot++)
			{
				u8 mode = pdata->padData[port][slot].mode;

				if (mode != MODE_DIGITAL && mode != MODE_ANALOG && mode != MODE_DS2_NATIVE)
				{
					break;
				}

				memcpy(&pads[port][slot], &pdata->padData[port][slot], sizeof(PadFreezeData));
			}

			if (pdata->slot[port] < 4)
				slots[port] = pdata->slot[port];
		}
	}
	else if (mode == FreezeAction::Save)
	{
		if (data->size != sizeof(PadFullFreezeData))
			return 0;

		PadFullFreezeData* pdata = (PadFullFreezeData*)(data->data);

		// Tales of the Abyss - pad fix
		// - PCSX2 only saves port0 (save #1), then port1 (save #2)

		memset(pdata, 0, data->size);
		strncpy(pdata->format, "LinPad", sizeof(pdata->format));
		pdata->version = PAD_SAVE_STATE_VERSION;
		pdata->query = query;

		for (int port = 0; port < 2; port++)
		{
			for (int slot = 0; slot < 4; slot++)
			{
				pdata->padData[port][slot] = pads[port][slot];
			}

			pdata->slot[port] = slots[port];
		}
	}
	else
	{
		return -1;
	}

	return 0;
}

u8 PADstartPoll(int pad)
{
	return pad_start_poll(pad);
}

u8 PADpoll(u8 value)
{
	return pad_poll(value);
}

void PAD::PollDevices()
{
#ifdef SDL_BUILD
	// Take the opportunity to handle hot plugging here
	SDL_Event events;
	while (SDL_PollEvent(&events))
	{
		switch (events.type)
		{
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED:
				EnumerateDevices();
				break;
			default:
				break;
		}
	}
#endif

	device_manager.Update();
}

/// g_key_status.press but with proper handling for analog buttons
static void PressButton(u32 pad, u32 button)
{
	// Analog controls.
	if (IsAnalogKey(button))
	{
		switch (button)
		{
			case PAD_R_LEFT:
			case PAD_R_UP:
			case PAD_L_LEFT:
			case PAD_L_UP:
				g_key_status.press(pad, button, -MAX_ANALOG_VALUE);
				break;
			case PAD_R_RIGHT:
			case PAD_R_DOWN:
			case PAD_L_RIGHT:
			case PAD_L_DOWN:
				g_key_status.press(pad, button, MAX_ANALOG_VALUE);
				break;
		}
	}
	else
	{
		g_key_status.press(pad, button);
	}
}

bool PAD::HandleHostInputEvent(const HostKeyEvent& event)
{
	switch (event.type)
	{
		case HostKeyEvent::Type::KeyPressed:
		case HostKeyEvent::Type::KeyReleased:
		{
			bool result = false;

			for (u32 cpad = 0; cpad < GAMEPAD_NUMBER; cpad++)
			{
				const int button_index = get_keyboard_key(cpad, event.key);
				if (button_index < 0)
					continue;

				g_key_status.keyboard_state_acces(cpad);
				if (event.type == HostKeyEvent::Type::KeyPressed)
					PressButton(cpad, button_index);
				else
					g_key_status.release(cpad, button_index);
			}

			return result;
		}
		break;

		default:
			return false;
	}
}

void PAD::LoadConfig(const SettingsInterface& si)
{
	g_conf.init();

	// load keyboard bindings
	for (u32 pad = 0; pad < GAMEPAD_NUMBER; pad++)
	{
		const std::string section(StringUtil::StdStringFromFormat("Pad%u", pad));
		std::string value;

		for (u32 button = 0; button < MAX_KEYS; button++)
		{
			const std::string config_key(StringUtil::StdStringFromFormat("Button%u", button));
			if (!si.GetStringValue(section.c_str(), config_key.c_str(), &value) || value.empty())
				continue;

			std::optional<u32> code = Host::ConvertKeyStringToCode(value);
			if (!code.has_value())
				continue;

			set_keyboard_key(pad, code.value(), static_cast<int>(button));
		}

		g_conf.set_joy_uid(pad, si.GetUIntValue(section.c_str(), "JoystickUID", 0u));
		g_conf.pad_options[pad].forcefeedback = si.GetBoolValue(section.c_str(), "ForceFeedback", true);
		g_conf.pad_options[pad].reverse_lx = si.GetBoolValue(section.c_str(), "ReverseLX", false);
		g_conf.pad_options[pad].reverse_ly = si.GetBoolValue(section.c_str(), "ReverseLY", false);
		g_conf.pad_options[pad].reverse_rx = si.GetBoolValue(section.c_str(), "ReverseRX", false);
		g_conf.pad_options[pad].reverse_ry = si.GetBoolValue(section.c_str(), "ReverseRY", false);
		g_conf.pad_options[pad].mouse_l = si.GetBoolValue(section.c_str(), "MouseL", false);
		g_conf.pad_options[pad].mouse_r = si.GetBoolValue(section.c_str(), "MouseR", false);
	}

	g_conf.set_sensibility(si.GetUIntValue("Pad", "MouseSensibility", 100));
	g_conf.set_ff_intensity(si.GetUIntValue("Pad", "FFIntensity", 0x7FFF));
}

static void SetKeyboardBinding(SettingsInterface& si, u32 port, const char* name, int binding)
{
	const std::string section(StringUtil::StdStringFromFormat("Pad%u", port));
	const std::string key(StringUtil::StdStringFromFormat("Button%d", binding));
	si.SetStringValue(section.c_str(), key.c_str(), name);
}

void PAD::SetDefaultConfig(SettingsInterface& si)
{
	SetKeyboardBinding(si, 0, "1", PAD_L2);
	SetKeyboardBinding(si, 0, "Q", PAD_R2);
	SetKeyboardBinding(si, 0, "E", PAD_L1);
	SetKeyboardBinding(si, 0, "3", PAD_R1);
	SetKeyboardBinding(si, 0, "I", PAD_TRIANGLE);
	SetKeyboardBinding(si, 0, "L", PAD_CIRCLE);
	SetKeyboardBinding(si, 0, "K", PAD_CROSS);
	SetKeyboardBinding(si, 0, "J", PAD_SQUARE);
	SetKeyboardBinding(si, 0, "Backspace", PAD_SELECT);
	SetKeyboardBinding(si, 0, "Return", PAD_START);
	SetKeyboardBinding(si, 0, "Up", PAD_UP);
	SetKeyboardBinding(si, 0, "Right", PAD_RIGHT);
	SetKeyboardBinding(si, 0, "Down", PAD_DOWN);
	SetKeyboardBinding(si, 0, "Left", PAD_LEFT);
	SetKeyboardBinding(si, 0, "W", PAD_L_UP);
	SetKeyboardBinding(si, 0, "D", PAD_L_RIGHT);
	SetKeyboardBinding(si, 0, "S", PAD_L_DOWN);
	SetKeyboardBinding(si, 0, "A", PAD_L_LEFT);
	SetKeyboardBinding(si, 0, "T", PAD_R_UP);
	SetKeyboardBinding(si, 0, "H", PAD_R_RIGHT);
	SetKeyboardBinding(si, 0, "G", PAD_R_DOWN);
	SetKeyboardBinding(si, 0, "F", PAD_R_LEFT);
}
