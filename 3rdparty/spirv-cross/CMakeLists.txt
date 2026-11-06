# Simplified CMakeLists for use with PCSX2

cmake_minimum_required(VERSION 3.10...4.0)

project(SPIRV-Cross LANGUAGES CXX)

add_library(spirv-cross-defines INTERFACE)
add_library(spirv-cross-core
	GLSL.std.450.h
	spirv_common.hpp
	spirv_cross_containers.hpp
	spirv_cross_error_handling.hpp
	spirv.hpp
	spirv_cross.hpp
	spirv_cross.cpp
	spirv_parser.hpp
	spirv_parser.cpp
	spirv_cross_parsed_ir.hpp
	spirv_cross_parsed_ir.cpp
	spirv_cfg.hpp
	spirv_cfg.cpp
)
add_library(spirv-cross-glsl
	spirv_glsl.cpp
	spirv_glsl.hpp
)
add_library(spirv-cross-msl
	spirv_msl.cpp
	spirv_msl.hpp
)
add_library(spirv-cross-hlsl
	spirv_hlsl.cpp
	spirv_hlsl.hpp
)

target_compile_definitions(spirv-cross-defines INTERFACE SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS)
if(CMAKE_CXX_COMPILER_ID MATCHES Clang)
	target_compile_options(spirv-cross-defines INTERFACE -Wno-deprecated-this-capture)
endif()

target_compile_features(spirv-cross-core PUBLIC cxx_std_11)
target_include_directories(spirv-cross-core PUBLIC .)
target_link_libraries(spirv-cross-core    PRIVATE spirv-cross-defines)
target_link_libraries(spirv-cross-glsl    PRIVATE spirv-cross-defines PUBLIC spirv-cross-core)
target_link_libraries(spirv-cross-msl     PRIVATE spirv-cross-defines PUBLIC spirv-cross-glsl)
target_link_libraries(spirv-cross-hlsl    PRIVATE spirv-cross-defines PUBLIC spirv-cross-glsl)
