cmake_minimum_required(VERSION 3.0)
set(project_name cinatra)
project(${project_name})
#set(CMAKE_BUILD_TYPE Release)
if (MSVC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++latest")
else ()
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -pthread -std=c++17")
endif ()

#option(ENABLE_SSL   "Set to switch to build use ssl"  OFF)
#option(ENABLE_GZIP  "Set to switch to build use gzip" OFF)
SET(ENABLE_GZIP OFF)
SET(ENABLE_SSL OFF)
SET(ENABLE_CLIENT_SSL OFF)
if (ENABLE_SSL)
	add_definitions(-DCINATRA_ENABLE_SSL)
	message(STATUS "Use SSL")
endif()

if(ENABLE_GZIP)
	add_definitions(-DCINATRA_ENABLE_GZIP)
endif()

if(ENABLE_CLIENT_SSL)
	add_definitions(-DCINATRA_ENABLE_CLIENT_SSL)
endif()

find_package(Boost 1.60 REQUIRED COMPONENTS system)
if (ENABLE_SSL)
find_package(OpenSSL REQUIRED)
endif()
if (ENABLE_CLIENT_SSL)
	find_package(OpenSSL REQUIRED)
endif()

if (ENABLE_GZIP)
	find_package(ZLIB REQUIRED)
endif()

set(CINATRA
	http_server.hpp
	connection.hpp
	io_service_pool.hpp
	nanolog.hpp
	picohttpparser.h
	request.hpp
	response.hpp
	utils.hpp
	response_cv.hpp
	itoa.hpp
	multipart_parser.hpp
	multipart_reader.hpp
	sha1.hpp
	websocket.hpp
	ws_define.h
	function_traits.hpp
	http_router.hpp
	router.hpp
	define.h
	url_encode_decode.hpp
	cookie.hpp
	session.hpp
	session_manager.hpp
	nlohmann_json.hpp
	render.h
	main.cpp
	)

add_executable(${project_name} ${CINATRA})
include_directories(${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})

target_link_libraries(${project_name} ${Boost_LIBRARIES} uuid -lstdc++fs)
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
