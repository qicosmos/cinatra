cmake_minimum_required(VERSION 3.9)
project(cinatra)

if (MSVC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++latest")
else ()
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -std=c++17")
endif ()

find_package(Boost 1.60 REQUIRED COMPONENTS system date_time)
find_package(OpenSSL REQUIRED)

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
include_directories(${Boost_INCLUDE_DIRS})
target_link_libraries(cinatra ${Boost_LIBRARIES} ${OPENSSL_LIBRARIES})
