// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <optional>

#ifndef __has_builtin
	#define __has_builtin(x) 0
#endif

#if !__has_builtin(__builtin_mul_overflow)
	#include <intrin.h>
#endif

#if !__has_builtin(__builtin_mul_overflow) || !__has_builtin(__builtin_add_overflow) || !__has_builtin(__builtin_sub_overflow)
	#include <type_traits>
#endif

namespace Common
{
	template <typename T>
	static constexpr inline __fi std::optional<T> CheckedMul(T a, T b)
	{
#if __has_builtin(__builtin_mul_overflow)
		T c;
		if (__builtin_mul_overflow(a, b, &c))
			return std::nullopt;
		return c;
#else
		static_assert(sizeof(T) <= sizeof(u64));
		if constexpr (sizeof(T) == sizeof(u64))
		{
			if constexpr (std::is_signed_v<T>)
			{
				s64 ch = __mulh(a, b);
				s64 c = a * b;
				if (ch != c >> 63)
					return std::nullopt;
				return c;
			}
			else
			{
				u64 ch = __umulh(a, b);
				u64 c = a * b;
				if (ch)
					return std::nullopt;
				return c;
			}
		}
		else
		{
			using i64 = std::conditional_t<std::is_signed_v<T>, s64, u64>;
			i64 c64 = static_cast<i64>(a) * static_cast<i64>(b);
			T c = static_cast<T>(c64);
			if (c != c64)
				return std::nullopt;
			return c;
		}
#endif
	}

	template <typename T>
	static constexpr inline __fi std::optional<T> CheckedAdd(T a, T b)
	{
#if __has_builtin(__builtin_add_overflow)
		T c;
		if (__builtin_add_overflow(a, b, &c))
			return std::nullopt;
		return c;
#else
		if constexpr (std::is_signed_v<T>)
		{
			if constexpr (sizeof(T) >= sizeof(u64))
			{
				using UT = std::make_unsigned_t<T>;
				UT uc = static_cast<UT>(a) + static_cast<UT>(b);
				UT highbit = static_cast<UT>(1) << (sizeof(T) * 8 - 1);
				if ((~(a & b) & (a ^ uc)) & highbit)
					return std::nullopt;
				return static_cast<T>(uc);
			}
			else
			{
				s64 c64 = static_cast<s64>(a) + static_cast<s64>(b);
				T c = static_cast<T>(c64);
				if (c != c64)
					return std::nullopt;
				return c;
			}
		}
		else
		{
			T c = a + b;
			if (c < a)
				return std::nullopt;
			return c;
		}
#endif
	}

	template <typename T>
	static constexpr inline __fi std::optional<T> CheckedSub(T a, T b)
	{
#if __has_builtin(__builtin_sub_overflow)
		T c;
		if (__builtin_sub_overflow(a, b, &c))
			return std::nullopt;
		return c;
#else
		if constexpr (std::is_signed_v<T>)
		{
			if constexpr (sizeof(T) >= sizeof(u64))
			{
				using UT = std::make_unsigned_t<T>;
				UT uc = static_cast<UT>(a) - static_cast<UT>(b);
				UT highbit = static_cast<UT>(1) << (sizeof(T) * 8 - 1);
				if (((a ^ b) & (a ^ uc)) & highbit)
					return std::nullopt;
				return static_cast<T>(uc);
			}
			else
			{
				s64 c64 = static_cast<s64>(a) - static_cast<s64>(b);
				T c = static_cast<T>(c64);
				if (c != c64)
					return std::nullopt;
				return c;
			}
		}
		else
		{
			if (a < b)
				return std::nullopt;
			return a - b;
		}
#endif
	}
} // namespace Common
