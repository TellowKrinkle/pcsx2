// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: MIT

#pragma once

#include <rashader-lite.h>

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_map>

#ifdef _MSC_VER
	#define NOINLINE __declspec(noinline)
	#define ALWAYS_INLINE __forceinline
#else
	#define NOINLINE __attribute__((noinline))
	#define ALWAYS_INLINE __attribute__((always_inline))
#endif

namespace rashader {
namespace detail {

std::optional<size_t> StringToEnum(const uint16_t* offsets, size_t noffsets, const char* lookup);
std::optional<size_t> StringToEnum(const uint16_t* offsets, size_t noffsets, std::string_view lookup);
static const char* EnumToString(const uint16_t* offsets, size_t noffsets, uint32_t lookup) {
	const char* base = reinterpret_cast<const char*>(offsets);
	return lookup < noffsets ? base + offsets[lookup] : nullptr;
}

} // namespace detail

template <typename Enum, size_t CharCount>
struct EnumLookup {
	/// List of offsets into this struct for the associated string to each enum value
	uint16_t name_offsets[static_cast<uint32_t>(Enum::Last) + 1] = {};
	/// Name string data
	char name_data[CharCount] = {};
	constexpr explicit EnumLookup(std::string_view fn(Enum)) {
		int data_off = 0;
		for (uint32_t i = 0; i <= static_cast<uint32_t>(Enum::Last); i++) {
			name_offsets[i] = offsetof(EnumLookup, name_data) + data_off;
			std::string_view name = fn(static_cast<Enum>(i));
			for (uint32_t j = 0; j < name.size(); j++) {
				name_data[data_off++] = name[j];
			}
			name_data[data_off++] = '\0';
		}
	}

	std::optional<Enum> ToEnum(const char* lookup) const {
		auto res = detail::StringToEnum(name_offsets, std::size(name_offsets), lookup);
		return res ? std::make_optional(static_cast<Enum>(*res)) : std::nullopt;
	}

	std::optional<Enum> ToEnum(std::string_view lookup) const {
		auto res = detail::StringToEnum(name_offsets, std::size(name_offsets), lookup);
		return res ? std::make_optional(static_cast<Enum>(*res)) : std::nullopt;
	}

	const char* ToString(Enum val) const {
		return detail::EnumToString(name_offsets, std::size(name_offsets), static_cast<uint32_t>(val));
	}
};

namespace detail {

template <auto Fn> struct EnumLookupMeta;
template <typename Enum, std::string_view Fn(Enum)>
struct EnumLookupMeta<Fn> {
	static constexpr size_t CharCount() {
		size_t res = 0;
		for (uint32_t i = 0; i <= static_cast<uint32_t>(Enum::Last); i++) {
			res += Fn(static_cast<Enum>(i)).size() + 1;
		}
		return res;
	}
	typedef EnumLookup<Enum, CharCount()> type;
};

} // namespace detail

template <auto Fn>
static constexpr typename rashader::detail::EnumLookupMeta<Fn>::type make_enum_lookup() {
	return typename rashader::detail::EnumLookupMeta<Fn>::type(Fn);
}

namespace parse {
static bool isalpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool isdigit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool isnewline(unsigned char c) { return c == '\r' || c == '\n'; }
static bool iswhitespace(unsigned char c) { return c == ' ' || c == '\t'; }

static bool has_suffix(std::string_view line, std::string_view suffix) {
	return line.size() >= suffix.size() && 0 == memcmp(line.data() + line.size() - suffix.size(), suffix.data(), suffix.size());
}

static bool try_consume_suffix(std::string_view* line, std::string_view prefix) {
	if (has_suffix(*line, prefix)) {
		line->remove_suffix(prefix.size());
		return true;
	}
	return false;
}

static bool has_prefix(std::string_view line, std::string_view prefix) {
	return line.size() >= prefix.size() && 0 == memcmp(line.data(), prefix.data(), prefix.size());
}

static bool try_consume_prefix(std::string_view* line, std::string_view prefix) {
	if (has_prefix(*line, prefix)) {
		line->remove_prefix(prefix.size());
		return true;
	}
	return false;
}

std::optional<uint64_t> parse_uint64_full(std::string_view line, int base = 0);
std::optional<uint64_t> parse_uint64_suffix(std::string_view* line_);
} // namespace parse

struct transparent_string_hash
{
	using is_transparent = void;

	std::size_t operator()(const std::string_view& v) const { return std::hash<std::string_view>{}(v); }
	std::size_t operator()(const std::string& s) const { return std::hash<std::string>{}(s); }
	std::size_t operator()(const char* s) const { return operator()(std::string_view(s)); }
};

struct transparent_string_equal
{
	using is_transparent = void;

	bool operator()(const std::string& lhs, const std::string& rhs) const { return lhs == rhs; }
	bool operator()(const std::string& lhs, const std::string_view& rhs) const { return lhs == rhs; }
	bool operator()(const std::string_view& lhs, const std::string& rhs) const { return lhs == rhs; }
	bool operator()(const std::string& lhs, const char* rhs) const { return lhs == rhs; }
	bool operator()(const char* lhs, const std::string& rhs) const { return lhs == rhs; }
};

typedef std::unordered_map<std::string, uint32_t, transparent_string_hash, transparent_string_equal> NameLookup;

} // namespace rashader
