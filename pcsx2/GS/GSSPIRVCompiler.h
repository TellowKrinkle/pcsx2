// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <optional>
#include <string_view>
#include <vector>

typedef struct shaderc_compiler* shaderc_compiler_t;

class GSSPIRVCompiler
{
	shaderc_compiler_t compiler;
public:
	// SPIR-V compiled code type
	using SPIRVCodeType = u32;
	using SPIRVCodeVector = std::vector<SPIRVCodeType>;

	enum class Type { VS, FS, CS };
	enum class DebugLevel { None, Base, NonSemanticDebugInfo };

	GSSPIRVCompiler();
	~GSSPIRVCompiler();
	std::optional<SPIRVCodeVector> Compile(Type type, std::string_view source, DebugLevel debug);
};
