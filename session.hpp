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
        session(const std::string& name,std::size_t expire, const std::string& path = "", const std::string& domain = ""):cookie_(name,"")
        {
            uuids::uuid_system_generator uid{};
            std::string uuid_str = uuids::to_string(uid());
            remove_char(uuid_str,'-');
            this->name = name;
            this->id = uuid_str;
            this->expire = expire;
            this->path = path;
            this->domain = domain;
            std::time_t time = getTimeStamp();
            this->timestamp = expire * 1000 + time;
            session::_threadLock.lock();
            std::shared_ptr<session> construct_ptr(this);
            GLOBAL_SESSION.insert(std::make_pair(this->id, construct_ptr));
            session::_threadLock.unlock();
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
        static std::shared_ptr<session> get(std::string id)
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
                std::time_t nowTimeStamp = session::getTimeStamp();
                for (auto iter = GLOBAL_SESSION.begin(); iter != GLOBAL_SESSION.end();)
                {
                    if (iter->second->timestamp < nowTimeStamp)
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
        const std::string get_name()
        {
            return name;
        }
        const std::string get_id()
        {
            return id;
        }
        const std::string get_path()
        {
            return path;
        }
        const std::string get_domain()
        {
            return domain;
        }
        void set_name(const std::string& _name)
        {
            session::_threadLock.lock();
            name = _name;
            session::_threadLock.unlock();
        }
        void set_id(const std::string& _id)
        {
            session::_threadLock.lock();
            id = _id;
            session::_threadLock.unlock();
        }
        void set_path(const std::string& _path)
        {
            session::_threadLock.lock();
            path = _path;
            session::_threadLock.unlock();
        }
        void set_domain(const std::string& _domain)
        {
            session::_threadLock.lock();
            domain = _domain;
            session::_threadLock.unlock();
        }
        void set_cookie()
        {
            session::_threadLock.lock();
            cookie_.set_version(0);
            cookie_.set_name(name);
            cookie_.set_value(id);
            cookie_.set_max_age(timestamp/1000);
            cookie_.set_domain(domain);
            cookie_.set_path(path);
            session::_threadLock.unlock();
        }
        cinatra::cookie get_cookie()
        {
            return cookie_;
        }
    public:
        static std::mutex _threadLock;
    private:
        std::string name;
        std::string id;
        std::string path;
        std::string domain;
        std::size_t expire;
        std::time_t timestamp;
        std::map<std::string, std::any> _data;
        cinatra::cookie cookie_{"",""};
    private:
        session()
        {

        }
    public:
        static std::time_t getTimeStamp()
        {
            std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> tp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
            auto tmp = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
            std::time_t timestamp = tmp.count();
            return timestamp;
        }
    };
    std::mutex session::_threadLock;
}

