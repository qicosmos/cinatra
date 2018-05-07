//
// Created by xmh on 18-5-7.
//

#ifndef CINATRA_SESSION_UTILS_HPP
#define CINATRA_SESSION_UTILS_HPP
#include "session.hpp"
#include "request.hpp"
namespace cinatra{

    inline static std::shared_ptr<session> create_session(const std::string& name,std::time_t time,const cinatra::request& req,const std::string &path = "/")
    {
        auto domain = req.get_header_value("host");
        auto pos = domain.find(":");
        if(pos!=std::string_view::npos)
        {
            domain = domain.substr(0,pos);
        }
        return session::make_session(name,time,path,std::string(domain.data(),domain.size()));
    }
}
#endif //CINATRA_SESSION_UTILS_HPP
