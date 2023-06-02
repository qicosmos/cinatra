set(project_name cinatra_example)
project(${project_name})

set(CINATRA_EXAMPLE
	main.cpp
	)

add_executable(${project_name} ${CINATRA_EXAMPLE})
target_compile_definitions(${project_name} PRIVATE ASYNC_SIMPLE_HAS_NOT_AIO)
target_include_directories(${project_name} PRIVATE
        ${cinatra_SOURCE_DIR}/thirdparty/asio
        ${cinatra_SOURCE_DIR}/thirdparty/async_simple
)

option(CINATRA_ENABLE_SSL "Enable ssl support" OFF)
if (CINATRA_ENABLE_SSL)
	message(STATUS "Use SSL")
	find_package(OpenSSL REQUIRED)
	add_definitions(-DCINATRA_ENABLE_SSL)
	target_link_libraries(${project_name} ${OPENSSL_LIBRARIES} pthread -ldl)
endif()

if (ENABLE_CLIENT_SSL)
	target_link_libraries(${project_name} ${OPENSSL_LIBRARIES} pthread -ldl)
endif()

if (ENABLE_GZIP)
	target_link_libraries(${project_name} ${ZLIB_LIBRARIES})
endif()
install(TARGETS ${project_name} DESTINATION include)
