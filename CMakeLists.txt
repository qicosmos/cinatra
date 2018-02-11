cmake_minimum_required(VERSION 3.0)
project(cinatra)

if (MSVC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++latest")
else ()
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -pthread -std=c++17")
endif ()

#option(ENABLE_SSL   "Set to switch to build use ssl"  OFF)
#option(ENABLE_GZIP  "Set to switch to build use gzip" OFF)
SET(ENABLE_SSL ON)
if (ENABLE_SSL)
	add_definitions(-DCINATRA_ENABLE_SSL)
	message(STATUS "Use SSL")
elseif(CINATRA_ENABLE_GZIP)
	add_definitions(-CINATRA_ENABLE_GZIP)
	message(STATUS "Use GZIP")
else()
	message(STATUS "Use Nothing")
endif()

find_package(Boost 1.60 REQUIRED COMPONENTS system)
if (ENABLE_SSL)
find_package(OpenSSL REQUIRED)
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
	main.cpp
	)

add_executable(cinatra ${CINATRA})
include_directories(${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})

target_link_libraries(cinatra ${Boost_LIBRARIES})
if (ENABLE_SSL)
	target_link_libraries(cinatra ${OPENSSL_LIBRARIES})
endif()
install(TARGETS cinatra DESTINATION include)
