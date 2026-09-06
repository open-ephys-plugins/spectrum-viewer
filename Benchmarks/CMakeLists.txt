include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	googlebenchmark
	GIT_REPOSITORY https://github.com/google/benchmark.git
	GIT_TAG v1.8.3
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(
	spectrum_viewer_benchmarks
	WindowPreprocessingBenchmarks.cpp)

target_compile_features(spectrum_viewer_benchmarks PRIVATE cxx_std_17)

if(MSVC)
	target_compile_options(spectrum_viewer_benchmarks PRIVATE /O2 /W4)
	if(BENCHMARK_NATIVE_ARCH)
		target_compile_options(spectrum_viewer_benchmarks PRIVATE /arch:AVX2)
	endif()
else()
	target_compile_options(spectrum_viewer_benchmarks PRIVATE -O3 -Wall -Wextra -Wpedantic)
	if(BENCHMARK_NATIVE_ARCH)
		target_compile_options(spectrum_viewer_benchmarks PRIVATE -march=native)
	endif()
endif()

target_link_libraries(spectrum_viewer_benchmarks PRIVATE benchmark::benchmark)
