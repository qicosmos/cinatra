#pragma once

#ifdef CINATRA_ENABLE_BOOST_ASIO

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>

#if defined(ENABLE_SSL) || defined(CINATRA_ENABLE_SSL)
#include <boost/asio/ssl.hpp>
#endif // defined(ENABLE_SSL) || defined(CINATRA_ENABLE_SSL)

#if defined(ENABLE_FILE_IO_URING)
#include <boost/asio/random_access_file.hpp>
#include <boost/asio/stream_file.hpp>
#endif

namespace asio_ns = boost::asio;

using asio_error_code = boost::system::error_code;

#elif defined(ASIO_STANDALONE) || defined(CINATRA_ENABLE_LOCAL_ASIO)

#define ASIO_STANDALONE

#include <asio.hpp>
#include <asio/error_code.hpp>
#include <asio/steady_timer.hpp>

#if defined(ENABLE_SSL) || defined(CINATRA_ENABLE_SSL)
#include <asio/ssl.hpp>
#endif // defined(ENABLE_SSL) || defined(CINATRA_ENABLE_SSL)

#if defined(ENABLE_FILE_IO_URING)
#include <asio/random_access_file.hpp>
#include <asio/stream_file.hpp>
#endif

namespace asio_ns = asio;

using asio_error_code = asio_ns::error_code;

#endif // CINATRA_ENABLE_BOOST_ASIO

using tcp_socket = asio_ns::ip::tcp::socket;

#if defined(ENABLE_SSL) || defined(CINATRA_ENABLE_SSL)
using ssl_socket = asio_ns::ssl::stream<asio_ns::ip::tcp::socket>;
#endif // defined(ENABLE_SSL) || defined(CINATRA_ENABLE_SSL)
