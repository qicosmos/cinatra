set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set(project_name test_cinatra)
add_executable(${project_name}
		test_coro_http_server.cpp
        test_cinatra.cpp
        test_cinatra_websocket.cpp
        test_cmdline.cpp
        main.cpp
        )
target_compile_definitions(${project_name} PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO INJECT_FOR_HTTP_CLIENT_TEST)
target_include_directories(${project_name} PRIVATE
        ${cinatra_SOURCE_DIR}/include
		${cinatra_SOURCE_DIR}/include/cinatra
)
if(ENABLE_METRIC_JSON)
	target_include_directories(${project_name} PRIVATE
	${cinatra_SOURCE_DIR}/iguana
	)
endif()

option(SKIP_TIME_TEST "skip time tests" OFF)
if(SKIP_TIME_TEST) 
	message(STATUS "skip time tests")
    add_definitions(-DSKIP_TIME_TEST)
endif()

add_test(NAME ${project_name} COMMAND test_cinatra)

if (ENABLE_GZIP)
	target_link_libraries(${project_name} ${ZLIB_LIBRARIES})
endif()

if (ENABLE_BROTLI)
	include_directories(${BROTLI_INCLUDE_DIRS})
	target_link_libraries(${project_name} ${BROTLI_LIBRARIES})
endif()

# test_coro_file
option(ENABLE_FILE_IO_URING "enable io_uring" OFF)
if(ENABLE_FILE_IO_URING)
    message(STATUS "enable io_uring")
    add_definitions(-DENABLE_FILE_IO_URING)
    add_definitions(-DASIO_HAS_FILE)
    add_definitions(-DASIO_HAS_IO_URING)
endif()

if(WIN32)
	add_definitions(-DASIO_HAS_IOCP)
endif()

option(USE_PREAD_WRITE "enable pread and pwrite" OFF)
if(USE_PREAD_WRITE)
	message(STATUS "use pread and pwrite")
    add_definitions(-DUSE_PREAD_WRITE)
endif()

if (UNIX)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -pthread")
endif()

## manual import
include_directories(${cinatra_SOURCE_DIR}/include)
include_directories(${cinatra_SOURCE_DIR}/iguana)

add_executable(test_corofile
        test_corofile.cpp
        )

if(ENABLE_FILE_IO_URING)
if (UNIX)
        target_link_libraries(test_corofile PRIVATE uring)
endif()
else()
        target_link_libraries(test_corofile PRIVATE)
endif()

add_test(NAME test_corofile COMMAND test_corofile)

add_executable(test_time_util
        test_time_util.cpp
        )
add_test(NAME test_time_util COMMAND test_time_util)

add_executable(test_metric
        test_metric.cpp
        )

if (ENABLE_SSL)
    message(STATUS "Use SSL")
    find_package(OpenSSL REQUIRED)
    add_definitions(-DCINATRA_ENABLE_SSL)
    target_link_libraries(test_cinatra  OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(test_corofile  OpenSSL::SSL OpenSSL::Crypto)
	target_link_libraries(test_metric  OpenSSL::SSL OpenSSL::Crypto)
endif ()

add_executable(test_http_parse
        test_http_parse.cpp
        )

if (ENABLE_SIMD STREQUAL "AARCH64")
	if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64")
		add_library(neon INTERFACE IMPORTED)
		target_compile_options(neon INTERFACE -march=armv8-a+fp+simd)
		target_link_libraries(test_http_parse neon)
	endif ()
elseif (ENABLE_SIMD STREQUAL "SSE42")
	if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
		add_library(sse4_2 INTERFACE IMPORTED)
		if(MSVC)
			target_compile_options(sse4_2 INTERFACE /arch:SSE4.2)
		else()
			target_compile_options(sse4_2 INTERFACE -msse4.2)
		endif()
		target_link_libraries(test_http_parse sse4_2)
	endif ()
elseif (ENABLE_SIMD STREQUAL "AVX2")
	if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
		add_library(avx2 INTERFACE IMPORTED)
		if(MSVC)
			target_compile_options(avx2 INTERFACE /arch:AVX2)
		else()
			target_compile_options(avx2 INTERFACE -mavx2)
		endif()
		target_link_libraries(test_http_parse avx2)
		set(CMAKE_CXX_FLAGS "-fpermissive")
	endif ()
endif ()

add_test(NAME test_http_parse COMMAND test_http_parse)
