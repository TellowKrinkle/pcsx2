/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#pragma once
#include "Pcsx2Defs.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

// Zero-extending helper
template <typename TReturn, typename TValue>
__fi constexpr TReturn ZeroExtend(TValue value)
{
	return static_cast<TReturn>(static_cast<typename std::make_unsigned<TReturn>::type>(
		static_cast<typename std::make_unsigned<TValue>::type>(value)));
}
// Sign-extending helper
template <typename TReturn, typename TValue>
__fi constexpr TReturn SignExtend(TValue value)
{
	return static_cast<TReturn>(
		static_cast<typename std::make_signed<TReturn>::type>(static_cast<typename std::make_signed<TValue>::type>(value)));
}

// Type-specific helpers
template <typename TValue>
__fi constexpr u16 ZeroExtend16(TValue value)
{
	return ZeroExtend<u16, TValue>(value);
}
template <typename TValue>
__fi constexpr u32 ZeroExtend32(TValue value)
{
	return ZeroExtend<u32, TValue>(value);
}
template <typename TValue>
__fi constexpr u64 ZeroExtend64(TValue value)
{
	return ZeroExtend<u64, TValue>(value);
}
template <typename TValue>
__fi constexpr u16 SignExtend16(TValue value)
{
	return SignExtend<u16, TValue>(value);
}
template <typename TValue>
__fi constexpr u32 SignExtend32(TValue value)
{
	return SignExtend<u32, TValue>(value);
}
template <typename TValue>
__fi constexpr u64 SignExtend64(TValue value)
{
	return SignExtend<u64, TValue>(value);
}
template <typename TValue>
__fi constexpr u8 Truncate8(TValue value)
{
	return static_cast<u8>(static_cast<typename std::make_unsigned<decltype(value)>::type>(value));
}
template <typename TValue>
__fi constexpr u16 Truncate16(TValue value)
{
	return static_cast<u16>(static_cast<typename std::make_unsigned<decltype(value)>::type>(value));
}
template <typename TValue>
__fi constexpr u32 Truncate32(TValue value)
{
	return static_cast<u32>(static_cast<typename std::make_unsigned<decltype(value)>::type>(value));
}

/// Returns the number of zero bits before the first set bit, going MSB->LSB.
template<typename T>
__fi unsigned CountLeadingZeros(T value)
{
#ifdef _MSC_VER
	if constexpr (sizeof(value) >= sizeof(u64))
	{
		unsigned long index;
		_BitScanReverse64(&index, ZeroExtend64(value));
		return static_cast<unsigned>(index) ^ static_cast<unsigned>((sizeof(value) * 8u) - 1u);
	}
	else
	{
		unsigned long index;
		_BitScanReverse(&index, ZeroExtend32(value));
		return static_cast<unsigned>(index) ^ static_cast<unsigned>((sizeof(value) * 8u) - 1u);
	}
#else
	if constexpr (sizeof(value) >= sizeof(u64))
		return static_cast<unsigned>(__builtin_clzl(ZeroExtend64(value)));
	else if constexpr (sizeof(value) == sizeof(u32))
		return static_cast<unsigned>(__builtin_clz(ZeroExtend32(value)));
	else
		return static_cast<unsigned>(__builtin_clz(ZeroExtend32(value))) & static_cast<unsigned>((sizeof(value) * 8u) - 1u);
#endif
}

/// Returns the number of zero bits before the first set bit, going LSB->MSB.
template<typename T>
__fi unsigned CountTrailingZeros(T value)
{
#ifdef _MSC_VER
	if constexpr (sizeof(value) >= sizeof(u64))
	{
		unsigned long index;
		_BitScanForward64(&index, ZeroExtend64(value));
		return index;
	}
	else
	{
		unsigned long index;
		_BitScanForward(&index, ZeroExtend32(value));
		return index;
	}
#else
	if constexpr (sizeof(value) >= sizeof(u64))
		return static_cast<unsigned>(__builtin_ctzl(ZeroExtend64(value)));
	else
		return static_cast<unsigned>(__builtin_ctz(ZeroExtend32(value)));
#endif
}
