//
// Created by xmh on 18-5-7.
//
#pragma once
#include <iostream>
#include <string>
#include <any>
#include <map>
#include <chrono>
#include <ctime>
#include <mutex>
#include <cstring>
#include "uuid.h"
#include <memory>
#include "cookie.hpp"
namespace cinatra{

    class session;
    static std::map<std::string, std::shared_ptr<session>> GLOBAL_SESSION;
    class session
    {
    public:
        static std::shared_ptr<session> make_session(const std::string& name,std::size_t expire, const std::string& path = "/", const std::string& domain = "")
        {
            uuids::uuid_system_generator uid{};
            std::string uuid_str = uuids::to_string(uid());
            remove_char(uuid_str,'-');
            std::shared_ptr<session> tmp_ptr = std::make_shared<session>(name,uuid_str,expire,path,domain);
            session::_threadLock.lock();
            GLOBAL_SESSION.insert(std::make_pair(uuid_str, tmp_ptr));
            session::_threadLock.unlock();
            return tmp_ptr;
        }
    public:
        session(const std::string& name,const std::string& uuid_str,std::size_t _expire, const std::string& path = "/", const std::string& domain = ""):cookie_("","")
        {
            this->id = uuid_str;
            this->expire = _expire==-1?600:_expire;
            std::time_t time = get_time_stamp();
            this->time_stamp = this->expire + time;
            cookie_.set_name(name);
            cookie_.set_path(path);
            cookie_.set_domain(domain);
            cookie_.set_value(uuid_str);
            cookie_.set_version(0);
            cookie_.set_max_age(_expire==-1?-1:time_stamp);
        }
        void set_data(const std::string& name, std::any data)
        {
            session::_threadLock.lock();
            _data[name] = data;
            session::_threadLock.unlock();
        }
        template<typename Type>
        Type get_data(const std::string& name)
        {
            auto itert = _data.find(name);
            if (itert != _data.end())
            {
                return std::any_cast<Type>(itert->second);
            }
            return Type{};
        }
    public:
        static std::shared_ptr<session> get(const std::string& id)
        {
            if (!GLOBAL_SESSION.empty())
            {
                auto iter = GLOBAL_SESSION.find(id);
                if (iter != GLOBAL_SESSION.end())
                {
                    return iter->second;
                }
                return nullptr;
            }
            return nullptr;
        }
        static std::map<std::string, std::shared_ptr<session>>::iterator del(std::map<std::string, std::shared_ptr<session>>::iterator iter)
        {
            return GLOBAL_SESSION.erase(iter);
        }
        static void tick_time()
        {
            session::_threadLock.lock();
            if (!GLOBAL_SESSION.empty())
            {
                std::time_t nowTimeStamp = session::get_time_stamp();
                for (auto iter = GLOBAL_SESSION.begin(); iter != GLOBAL_SESSION.end();)
                {
                    if (iter->second->time_stamp < nowTimeStamp)
                    {
                        iter = session::del(iter);
                    }
                    else {
                        iter++;
                    }
                }
            }
            session::_threadLock.unlock();
        }
        static void remove_char(std::string &src,const char rp)
        {
            for(int index=0;index<src.size();index++)
            {
                if(src[index]==rp)
                {
                    src.replace(index,1,"");
                }
            }
        }
    public:
        const std::string get_id()
        {
            return id;
        }
        const std::time_t get_max_age()
        {
            return expire;
        }
        void set_max_age(const std::time_t seconds)
        {
            session::_threadLock.lock();
            expire = seconds==-1?600:seconds;
            std::time_t time = get_time_stamp();
            time_stamp = expire + time;
            cookie_.set_max_age(seconds==-1?-1:time_stamp);
            session::_threadLock.unlock();
        }
        cinatra::cookie get_cookie()
        {
            return cookie_;
        }
    public:
        static std::mutex _threadLock;
    private:
        std::string id;
        std::size_t expire;
        std::time_t time_stamp;
        std::map<std::string, std::any> _data;
        cinatra::cookie cookie_;
    private:
        session()= delete;
    public:
        static std::time_t get_time_stamp()
        {
            return std::time(nullptr);
//            std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> tp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
//            auto tmp = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
//            std::time_t timestamp = tmp.count();
//            return timestamp;
        }
    };
    std::mutex session::_threadLock;
}

