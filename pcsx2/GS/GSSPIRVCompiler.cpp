// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSSPIRVCompiler.h"

#include <shaderc/shaderc.h>

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "Config.h"

#include <mutex>

#define SHADERC_FUNCTIONS(X) \
	X(shaderc_compiler_initialize) \
	X(shaderc_compiler_release) \
	X(shaderc_compile_options_initialize) \
	X(shaderc_compile_options_release) \
	X(shaderc_compile_options_set_source_language) \
	X(shaderc_compile_options_set_generate_debug_info) \
	X(shaderc_compile_options_set_optimization_level) \
	X(shaderc_compile_options_set_target_env) \
	X(shaderc_compile_into_spv) \
	X(shaderc_result_release) \
	X(shaderc_result_get_length) \
	X(shaderc_result_get_num_warnings) \
	X(shaderc_result_get_bytes) \
	X(shaderc_result_get_error_message) \
	X(shaderc_result_get_compilation_status)

namespace dyn_shaderc
{
	static void Open();
	static void Close();

	static DynamicLibrary s_library;
	static std::once_flag s_open_once;

#define ADD_FUNC(F) static decltype(&::F) F;
	SHADERC_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

} // namespace dyn_shaderc

void dyn_shaderc::Open()
{
	Error error;

#ifdef _WIN32
	const std::string libname = DynamicLibrary::GetVersionedFilename("shaderc_shared");
#else
	// Use versioned, bundle post-processing adds it..
	const std::string libname = DynamicLibrary::GetVersionedFilename("shaderc_shared", 1);
#endif
	if (!s_library.Open(libname.c_str(), &error))
	{
		ERROR_LOG("Failed to load shaderc: {}", error.GetDescription());
		return;
	}

#define LOAD_FUNC(F) \
	if (!s_library.GetSymbol(#F, &F)) \
	{ \
		ERROR_LOG("Failed to find function {}", #F); \
		Close(); \
		return; \
	}

	SHADERC_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

	std::atexit(&dyn_shaderc::Close);
}

void dyn_shaderc::Close()
{
#define UNLOAD_FUNC(F) F = nullptr;
	SHADERC_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

	s_library.Close();
}

#undef SHADERC_FUNCTIONS
#undef SHADERC_INIT_FUNCTIONS

GSSPIRVCompiler::GSSPIRVCompiler()
{
	std::call_once(dyn_shaderc::s_open_once, dyn_shaderc::Open);
	if (dyn_shaderc::s_library.IsOpen())
		compiler = dyn_shaderc::shaderc_compiler_initialize();
}

GSSPIRVCompiler::~GSSPIRVCompiler()
{
	if (compiler)
		dyn_shaderc::shaderc_compiler_release(compiler);
}

static shaderc_shader_kind Convert(GSSPIRVCompiler::Type type)
{
	switch (type) {
		case GSSPIRVCompiler::Type::VS: return shaderc_vertex_shader;
		case GSSPIRVCompiler::Type::FS: return shaderc_fragment_shader;
		case GSSPIRVCompiler::Type::CS: return shaderc_compute_shader;
	}
}

static const char* compilation_status_to_string(shaderc_compilation_status status)
{
	switch (status)
	{
#define CASE(x) case shaderc_compilation_status_##x: return #x
		CASE(success);
		CASE(invalid_stage);
		CASE(compilation_error);
		CASE(internal_error);
		CASE(null_result_object);
		CASE(invalid_assembly);
		CASE(validation_error);
		CASE(transformation_error);
		CASE(configuration_error);
#undef CASE
	}
	return "unknown_error";
}

void DumpBadShader(std::string_view code, std::string_view errors)
{
	static u32 s_next_bad_shader_id = 0;
	const std::string filename = Path::Combine(EmuFolders::Logs, fmt::format("pcsx2_bad_shader_{}.txt", ++s_next_bad_shader_id));
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wb");
	if (fp)
	{
		if (!code.empty())
			std::fwrite(code.data(), code.size(), 1, fp.get());
		std::fputs("\n\n**** ERRORS ****\n", fp.get());
		if (!errors.empty())
			std::fwrite(errors.data(), errors.size(), 1, fp.get());
	}
	Console.ErrorFmt("Bad shader written to {}", filename);
}

std::optional<GSSPIRVCompiler::SPIRVCodeVector> GSSPIRVCompiler::Compile(Type type, std::string_view source, DebugLevel debug)
{
	std::optional<SPIRVCodeVector> ret = std::nullopt;

	if (!compiler)
		return std::nullopt;

	shaderc_compile_options_t options = dyn_shaderc::shaderc_compile_options_initialize();
	pxAssertRel(options, "shaderc_compile_options_initialize() failed");

	dyn_shaderc::shaderc_compile_options_set_source_language(options, shaderc_source_language_glsl);
	dyn_shaderc::shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, 0);
#ifdef SHADERC_PCSX2_CUSTOM
	dyn_shaderc::shaderc_compile_options_set_generate_debug_info(options, debug >= DebugLevel::Base, debug >= DebugLevel::NonSemanticDebugInfo);
#else
	if (debug >= DebugLevel::Base)
		dyn_shaderc::shaderc_compile_options_set_generate_debug_info(options);
#endif
	dyn_shaderc::shaderc_compile_options_set_optimization_level(
		options, debug >= DebugLevel::Base ? shaderc_optimization_level_zero : shaderc_optimization_level_performance);

	const shaderc_compilation_result_t result = dyn_shaderc::shaderc_compile_into_spv(
		compiler, source.data(), source.length(), Convert(type), "source", "main", options);

	shaderc_compilation_status status = shaderc_compilation_status_null_result_object;
	if (!result || (status = dyn_shaderc::shaderc_result_get_compilation_status(result)) != shaderc_compilation_status_success)
	{
		const std::string_view errors = result ? dyn_shaderc::shaderc_result_get_error_message(result)
		                                       : "null result object";
		ERROR_LOG("Failed to compile shader to SPIR-V: {}\n{}", compilation_status_to_string(status), errors);
		DumpBadShader(source, errors);
	}
	else
	{
		const size_t num_warnings = dyn_shaderc::shaderc_result_get_num_warnings(result);
		if (num_warnings > 0)
			WARNING_LOG("Shader compiled with warnings:\n{}", dyn_shaderc::shaderc_result_get_error_message(result));

		const size_t spirv_size = dyn_shaderc::shaderc_result_get_length(result);
		const char* bytes = dyn_shaderc::shaderc_result_get_bytes(result);
		pxAssert(spirv_size > 0 && ((spirv_size % sizeof(u32)) == 0));
		ret = SPIRVCodeVector(reinterpret_cast<const u32*>(bytes), reinterpret_cast<const u32*>(bytes + spirv_size));
	}

	dyn_shaderc::shaderc_result_release(result);
	dyn_shaderc::shaderc_compile_options_release(options);

	return ret;
}
