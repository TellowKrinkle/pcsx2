// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: MIT

#include "common.h"

NOINLINE std::optional<size_t> rashader::detail::StringToEnum(const uint16_t* offsets, size_t noffsets, const char* lookup) {
	const char* base = reinterpret_cast<const char*>(offsets);
	for (uint32_t i = 0; i < noffsets; i++) {
		const char* str = base + offsets[i];
		if (0 == strcmp(str, lookup)) {
			return i;
		}
	}
	return std::nullopt;
}

NOINLINE std::optional<size_t> rashader::detail::StringToEnum(const uint16_t* offsets, size_t noffsets, std::string_view lookup) {
	const char* base = reinterpret_cast<const char*>(offsets);
	for (uint32_t i = 0; i < noffsets; i++) {
		const char* str = base + offsets[i];
		if (0 == strncmp(str, lookup.data(), lookup.size()) && str[lookup.size()] == '\0') {
			return i;
		}
	}
	return std::nullopt;
}

std::optional<uint64_t> rashader::parse::parse_uint64_full(std::string_view line, int base) {
	if (line.empty())
		return std::nullopt;
	char* end;
	const char* beg = line.data();
	uint64_t res = std::strtoull(beg, &end, base);
	if (end == line.data() + line.size())
		return res;
	return std::nullopt;
}

std::optional<uint64_t> rashader::parse::parse_uint64_suffix(std::string_view* line_) {
	std::string_view line = *line_;
	size_t split = line.size();
	while (split > 0 && parse::isdigit(line[split - 1]))
		--split;
	if (split == line.size())
		return std::nullopt;
	uint64_t res = parse_uint64_full(std::string_view(line.data() + split, line.size() - split), 10).value_or(~0ull);
	*line_ = std::string_view(line.data(), split);
	return res;
}
