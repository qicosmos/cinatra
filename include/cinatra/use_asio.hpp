#pragma once

#if defined(ASIO_STANDALONE)
//MSVC : define environment path 'ASIO_STANDALONE_INCLUDE', e.g. 'E:\bdlibs\asio-1.10.6\include'

#include <asio.hpp>
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
#else
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#endif
