//
// Created by xmh on 18-5-7.
//

#ifndef CINATRA_SESSION_UTILS_HPP
#define CINATRA_SESSION_UTILS_HPP
#include "session.hpp"
#include "request.hpp"
namespace cinatra {

	inline static std::shared_ptr<session> create_session(const request& req, const std::string& name, std::time_t expire = -1, const std::string &path = "/")
	{
		auto domain = req.get_header_value("host");
		auto pos = domain.find(":");
		if (pos != std::string_view::npos)
		{
			domain = domain.substr(0, pos);
		}
		return session::make_session(name, expire, path, std::string(domain.data(), domain.length()));
	}
}
#endif //CINATRA_SESSION_UTILS_HPP
