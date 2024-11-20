# extra
option(BUILD_EXAMPLES "Build examples" ON)

# bench test
option(BUILD_BENCHMARK "Build benchmark" ON)

# unit test
option(BUILD_UNIT_TESTS "Build unit tests" ON)
if(BUILD_UNIT_TESTS)
    enable_testing()
endif()

# press tool
option(BUILD_PRESS_TOOL "Build press tool" ON)

# coverage test
option(COVERAGE_TEST "Build with unit test coverage" OFF)
if(COVERAGE_TEST)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage --coverage")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
    endif()
endif()

macro(check_asan _RESULT)
    include(CheckCXXSourceRuns)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
    check_cxx_source_runs(
            [====[
int main()
{
  return 0;
}
]====]
            ${_RESULT}
    )
    unset(CMAKE_REQUIRED_FLAGS)
endmacro()

# Enable address sanitizer
option(ENABLE_SANITIZER "Enable sanitizer(Debug+Gcc/Clang/AppleClang)" ON)
if(ENABLE_SANITIZER AND NOT MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        check_asan(HAS_ASAN)
        if(HAS_ASAN)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
        else()
            message(WARNING "sanitizer is no supported with current tool-chains")
        endif()
    else()
        message(WARNING "Sanitizer supported only for debug type")
    endif()
endif()

option(ENABLE_METRIC_JSON "Enable serialize metric to json" OFF)
if(ENABLE_METRIC_JSON)
    add_definitions(-DCINATRA_ENABLE_METRIC_JSON)
    message(STATUS "Enable serialize metric to json")
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")

SET(ENABLE_GZIP OFF)
SET(ENABLE_SSL OFF)
SET(ENABLE_CLIENT_SSL OFF)
SET(ENABLE_BROTLI OFF)

if (ENABLE_SSL)
	add_definitions(-DCINATRA_ENABLE_SSL)
	message(STATUS "Use SSL")
endif()

if(ENABLE_CLIENT_SSL)
	add_definitions(-DCINATRA_ENABLE_CLIENT_SSL)
endif()


if(ENABLE_SIMD STREQUAL "SSE42" OR ENABLE_SIMD STREQUAL "AVX2" OR ENABLE_SIMD STREQUAL "AARCH64")
	if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64")
		message(STATUS "Build with simd in aarch64")
		add_definitions(-DCINATRA_ARM_OPT)
	elseif (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
		message(STATUS "Build with simd in X86_64")
		if (ENABLE_SIMD STREQUAL "SSE42")
			message(STATUS "Build with SSE4.2 ISA")
			add_definitions(-DCINATRA_SSE)
		elseif (ENABLE_SIMD STREQUAL "AVX2")
			message(STATUS "Build with AVX2 ISA")
			add_definitions(-DCINATRA_AVX2)
		endif ()
	endif ()
endif()

add_definitions(-DASIO_STANDALONE)

if (ENABLE_SSL)
find_package(OpenSSL REQUIRED)
endif()
if (ENABLE_CLIENT_SSL)
	find_package(OpenSSL REQUIRED)
endif()

if (ENABLE_GZIP)
	find_package(ZLIB REQUIRED)
endif()

if (ENABLE_BROTLI)
	find_package(Brotli REQUIRED)
	if (Brotli_FOUND)
		message(STATUS "Brotli found")
		add_definitions(-DCINATRA_ENABLE_BROTLI)
	endif (Brotli_FOUND)
endif(ENABLE_BROTLI)


add_definitions(-DCORO_HTTP_PRINT_REQ_HEAD)