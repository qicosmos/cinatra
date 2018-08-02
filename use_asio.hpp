#pragma once
//MSVC : define environment path 'ASIO_STANDALONE_INCLUDE', e.g. 'E:\bdlibs\asio-1.10.6\include'

#include <asio.hpp>
#ifdef CINATRA_ENABLE_SSL
#include <asio/ssl.hpp>
#endif
#include <asio/steady_timer.hpp>
namespace boost
{
	namespace asio
	{
		using namespace ::asio;
	}
	namespace system
	{
		using ::std::error_code;
	}
}
using tcp_socket = boost::asio::ip::tcp::socket;
#ifdef CINATRA_ENABLE_SSL
using ssl_socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
#endif
