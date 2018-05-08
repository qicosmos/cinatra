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
namespace cinatra {

	class session;
	static std::map<std::string, std::shared_ptr<session>> GLOBAL_SESSION;
	class session
	{
	public:
		static std::shared_ptr<session> make_session(const std::string& name, std::size_t expire, const std::string& path = "/", const std::string& domain = "")
		{
			uuids::uuid_system_generator uid{};
			std::string uuid_str = uid().to_short_str();
			std::shared_ptr<session> tmp_ptr = std::make_shared<session>(name, uuid_str, expire, path, domain);
			session::_threadLock.lock();
			GLOBAL_SESSION.insert(std::make_pair(uuid_str, tmp_ptr));
			session::_threadLock.unlock();
			return tmp_ptr;
		}

		session(const std::string& name, const std::string& uuid_str, std::size_t _expire, const std::string& path = "/", const std::string& domain = "")
		{
			this->id_ = uuid_str;
			this->expire_ = _expire == -1 ? 600 : _expire;
			std::time_t time = std::time(nullptr);
			this->time_stamp_ = this->expire_ + time;
			cookie_.set_name(name);
			cookie_.set_path(path);
			cookie_.set_domain(domain);
			cookie_.set_value(uuid_str);
			cookie_.set_version(0);
			cookie_.set_max_age(_expire == -1 ? -1 : time_stamp_);
		}

		void set_data(const std::string& name, std::any data)
		{
			session::_threadLock.lock();
			data_[name] = std::move(data);
			session::_threadLock.unlock();
		}

		template<typename T>
		T get_data(const std::string& name)
		{
			auto itert = data_.find(name);
			if (itert != data_.end())
			{
				return std::any_cast<T>(itert->second);
			}
			return T{};
		}

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
				std::time_t nowTimeStamp = std::time(nullptr);
				for (auto iter = GLOBAL_SESSION.begin(); iter != GLOBAL_SESSION.end();)
				{
					if (iter->second->time_stamp_ < nowTimeStamp)
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

	public:
		const std::string get_id()
		{
			return id_;
		}
		const std::time_t get_max_age()
		{
			return expire_;
		}
		void set_max_age(const std::time_t seconds)
		{
			session::_threadLock.lock();
			expire_ = seconds == -1 ? 600 : seconds;
			std::time_t time = std::time(nullptr);
			time_stamp_ = expire_ + time;
			cookie_.set_max_age(seconds == -1 ? -1 : time_stamp_);
			session::_threadLock.unlock();
		}
		cinatra::cookie get_cookie()
		{
			return cookie_;
		}
	public:
		static std::mutex _threadLock;
	private:
		session() = delete;

		std::string id_;
		std::size_t expire_;
		std::time_t time_stamp_;
		std::map<std::string, std::any> data_;
		cinatra::cookie cookie_;
	};
	std::mutex session::_threadLock;
}

