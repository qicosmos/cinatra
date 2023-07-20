set(project_name cinatra_press_tool)
project(${project_name})

option(ENABLE_PRESS_TOOL_TESTS "Disable press tool test" OFF)

set(cinatra_press_tool
	main.cpp
	)

add_definitions(-DBENCHMARK_TEST)
add_executable(${project_name} ${cinatra_press_tool})
target_compile_definitions(${project_name} PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO)

if (ENABLE_SSL)
	target_link_libraries(${project_name} ${OPENSSL_LIBRARIES} pthread -ldl)
endif()

if (ENABLE_CLIENT_SSL)
	target_link_libraries(${project_name} ${OPENSSL_LIBRARIES} pthread -ldl)
endif()

if (ENABLE_GZIP)
	target_link_libraries(${project_name} ${ZLIB_LIBRARIES})
endif()

if (ENABLE_SIMD STREQUAL "AARCH64")
	if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64")
		#TODO
	endif ()
elseif (ENABLE_SIMD STREQUAL "SSE42")
	if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
		target_link_libraries(${project_name} sse4.2)
	endif ()
elseif (ENABLE_SIMD STREQUAL "AVX2")
	if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
		target_link_libraries(${project_name} avx2)
	endif ()
endif ()

install(TARGETS ${project_name} DESTINATION include)

set(unittest_press_tool test_cinatra_press_tool)

if (ENABLE_PRESS_TOOL_TESTS)
    add_executable(${unittest_press_tool} main.cpp)

    target_compile_definitions(${unittest_press_tool} PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO PRESS_TOOL_UNITTESTS)

    if (ENABLE_SSL)
        target_link_libraries(${unittest_press_tool} ${OPENSSL_LIBRARIES} pthread -ldl)
    endif()

    if (ENABLE_CLIENT_SSL)
        target_link_libraries(${unittest_press_tool} ${OPENSSL_LIBRARIES} pthread -ldl)
    endif()

    if (ENABLE_GZIP)
        target_link_libraries(${unittest_press_tool} ${ZLIB_LIBRARIES})
    endif()

endif()