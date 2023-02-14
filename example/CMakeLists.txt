set(project_name cinatra_example)
project(${project_name})

set(CINATRA_EXAMPLE
	main.cpp
	)

add_executable(${project_name} ${CINATRA_EXAMPLE})
target_include_directories(${project_name} PRIVATE
        ${cinatra_SOURCE_DIR}/thirdparty/asio
)

if(LINUX)
	target_link_libraries(${project_name} ${Boost_LIBRARIES} -lstdc++fs)
endif()
if (ENABLE_SSL)
	target_link_libraries(${project_name} ${OPENSSL_LIBRARIES} pthread -ldl)
endif()

if (ENABLE_CLIENT_SSL)
	target_link_libraries(${project_name} ${OPENSSL_LIBRARIES} pthread -ldl)
endif()

if (ENABLE_GZIP)
	target_link_libraries(${project_name} ${ZLIB_LIBRARIES})
endif()
install(TARGETS ${project_name} DESTINATION include)
