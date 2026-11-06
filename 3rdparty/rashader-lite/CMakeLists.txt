cmake_minimum_required(VERSION 3.10...4.3)
project(rashader-lite LANGUAGES CXX)

add_library(rashader-lite include/rashader-lite.h src/common.h src/common.cpp src/slang.cpp src/spirv.cpp src/spirv.hpp src/execution-planner.cpp)
target_include_directories(rashader-lite PUBLIC include)
target_compile_features(rashader-lite PUBLIC cxx_std_17 PRIVATE cxx_std_20)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	find_package(glslang)

	if(glslang_FOUND)
		add_executable(rashader-lite-tests test/tests.cpp)
		target_link_libraries(rashader-lite-tests PRIVATE rashader-lite glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
	else()
		message("glslang not found, disabling tests")
	endif()
endif()

add_library(rashader-lite::rashader-lite ALIAS rashader-lite)
