add_library(D3D12MemAlloc
	include/D3D12MemAlloc.h
	src/D3D12MemAlloc.cpp
)

target_include_directories(D3D12MemAlloc PUBLIC include)
target_compile_features(D3D12MemAlloc PUBLIC cxx_std_17)
