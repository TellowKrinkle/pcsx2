foreach(isa "sse4" "avx" "avx2")
	set(GSDir ${CMAKE_SOURCE_DIR}/pcsx2/GS)

	if(${native_vector_isa} LESS ${isa_number_${isa}})
		# Skip unsupported tests
		continue()
	endif()

	add_pcsx2_test(swizzle_test_${isa}
		swizzle_test_main.cpp
		swizzle_test_nops.cpp
		${GSDir}/GSBlock.cpp
		${GSDir}/GSBlock.h
		${GSDir}/GSClut.cpp
		${GSDir}/GSClut.h
		${GSDir}/GSTables.cpp
		${GSDir}/GSTables.h)

	target_include_directories(swizzle_test_${isa} PRIVATE ${GSDir} ${CMAKE_SOURCE_DIR}/pcsx2/ ${CMAKE_SOURCE_DIR}/pcsx2/gui)

	target_compile_options(swizzle_test_${isa} PRIVATE ${compile_options_${isa}})
	target_compile_definitions(swizzle_test_${isa} PRIVATE ${definitions_${isa}})
	if(WIN32)
		target_compile_definitions(swizzle_test_${isa} PRIVATE
			WINVER=0x0603
			_WIN32_WINNT=0x0603
			WIN32_LEAN_AND_MEAN
		)
	endif()
endforeach()

add_pcsx2_test(vertex_trace_test
	vertex_trace_test_main.cpp
	${GSDir}/GSVector.cpp
	${GSDir}/Renderers/Common/GSVertexTrace.cpp)
target_include_directories(vertex_trace_test PRIVATE ${GSDir} ${CMAKE_SOURCE_DIR}/pcsx2 ${CMAKE_SOURCE_DIR}/3rdparty/xbyak)
target_compile_options(vertex_trace_test PRIVATE ${compile_options_ninsse4})
target_compile_definitions(vertex_trace_test PRIVATE ${definitions_sse4})
