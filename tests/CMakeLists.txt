set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set(project_name test_cinatra)
add_executable(${project_name}
        test_cinatra.cpp
        test_cinatra_websocket.cpp
        test_cmdline.cpp
        main.cpp
        )
target_compile_definitions(${project_name} PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO INJECT_FOR_HTTP_CLIENT_TEST)
target_include_directories(${project_name} PRIVATE
        ${cinatra_SOURCE_DIR}/include
)

add_test(NAME ${project_name} COMMAND test_cinatra)

if (ENABLE_GZIP)
	target_link_libraries(${project_name} ${ZLIB_LIBRARIES})
endif()

# test_coro_file
option(ENABLE_FILE_IO_URING "enable io_uring" OFF)
if(ENABLE_FILE_IO_URING)
    message(STATUS "enable io_uring")
    add_definitions(-DENABLE_FILE_IO_URING)
    add_definitions(-DASIO_HAS_FILE)
    add_definitions(-DASIO_HAS_IO_URING)
endif()

if (UNIX)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

## manual import
include_directories(${cinatra_SOURCE_DIR}/include)

add_executable(test_corofile
        test_corofile.cpp
        # main.cpp
        )

if(ENABLE_FILE_IO_URING)
if (UNIX)
        target_link_libraries(test_corofile PRIVATE uring)
endif()
else()
        target_link_libraries(test_corofile PRIVATE)
endif()

add_test(NAME test_corofile COMMAND test_corofile)

option(CINATRA_ENABLE_SSL "Enable ssl support" OFF)
if (CINATRA_ENABLE_SSL)
    message(STATUS "Use SSL")
    find_package(OpenSSL REQUIRED)
    add_definitions(-DCINATRA_ENABLE_SSL)
    target_link_libraries(test_cinatra  OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(test_corofile  PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif ()