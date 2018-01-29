cmake_minimum_required(VERSION 2.8)
project(cinatra)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -std=c++17")
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE} -ldl")
set(IGUANA_DIR ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Boost 1.66 COMPONENTS coroutine context system thread filesystem REQUIRED)

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

add_executable(cinatra   ${CINATRA})
target_link_libraries(cinatra ${Boost_LIBRARIES} ${OPENSSL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIB})


