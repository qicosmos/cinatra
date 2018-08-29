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
#include "nlohmann_json.hpp"
#include "define.h"
namespace cinatra {

	class session
	{
	public:
		session() = default;
		session(const std::string& name, const std::string& uuid_str, std::size_t expire, 
			const std::string& path = "/", const std::string& domain = "")
		{
			id_ = uuid_str;
			expire_ = expire == -1 ? 86400 : expire;
			std::time_t now = std::time(nullptr);
			time_stamp_ = expire_ + now;
			cookie_.set_name(name);
			cookie_.set_path(path);
			cookie_.set_domain(domain);
			cookie_.set_value(uuid_str);
			cookie_.set_version(0);
			cookie_.set_max_age(expire == -1 ? -1 : time_stamp_);
		}
        template<typename T>
		void set_data(const std::string& name, T&& data)
		{
			{
				std::unique_lock<std::mutex> lock(mtx_);
				data_[name] = std::move(data);
			}
			write_session_to_file();
		}

		template<typename T>
		T get_data(const std::string& name)
		{
			std::unique_lock<std::mutex> lock(mtx_);
			auto itert = data_.find(name);
			if (itert != data_.end())
			{
				return (*itert).get<T>();
			}
			return T{};
		}

		bool has(const std::string& name) {
			std::unique_lock<std::mutex> lock(mtx_);
			return data_.find(name) != data_.end();
		}

		const std::string get_id()
		{
			std::unique_lock<std::mutex> lock(mtx_);
			return id_;
		}

		void set_max_age(const std::time_t seconds)
		{
			{
				std::unique_lock<std::mutex> lock(mtx_);
				is_update_ = true;
				expire_ = seconds == -1 ? 86400 : seconds;
				std::time_t now = std::time(nullptr);
				time_stamp_ = now + expire_;
				cookie_.set_max_age(seconds == -1 ? -1 : time_stamp_);
			}
			write_session_to_file();
		}

		void remove()
		{
			set_max_age(0);
		}

		cinatra::cookie& get_cookie()
		{
			return cookie_;
		}

		std::time_t time_stamp() {
			return time_stamp_;
		}

		bool is_need_update()
		{
			std::unique_lock<std::mutex> lock(mtx_);
			return is_update_;
		}

		void set_need_update(bool flag)
		{
			std::unique_lock<std::mutex> lock(mtx_);
			is_update_ = flag;
		}

		void write_session_to_file()
		{
			std::unique_lock<std::mutex> lock(mtx_);
			std::string file_path = std::string("./")+static_session_db_dir+"/"+id_;
			std::ofstream file(file_path,std::ios_base::out);
			if(file.is_open()){
				file << serialize_to_object();
			}
			file.close();
		}

		nlohmann::json serialize_to_object()
		{
			nlohmann::json root;
			root["id"] = id_;
			root["expire"] = expire_;
			root["time_stamp"] = time_stamp_;
			root["data"] = data_;
			root["cookie"]["version"] = cookie_.get_version();
			root["cookie"]["name"] = cookie_.get_name();
			root["cookie"]["value"] = cookie_.get_value();
			root["cookie"]["comment"] = cookie_.get_comment();
			root["cookie"]["domain"] =cookie_.get_domain();
			root["cookie"]["path"] = cookie_.get_path();
			root["cookie"]["priority"] = cookie_.get_priority();
			root["cookie"]["secure"] = cookie_.get_secure();
			root["cookie"]["max_age"] = cookie_.get_max_age();
			root["cookie"]["http_only"] = cookie_.get_http_only();
			return  root;
		}

		void serialize_from_object(nlohmann::json root)
		{
			std::unique_lock<std::mutex> lock(mtx_);
			id_ = root["id"];
			expire_ = root["expire"];
			time_stamp_ = root["time_stamp"];
			data_ = root["data"];
			cookie_.set_version(root["cookie"]["version"]);
			cookie_.set_name(root["cookie"]["name"]);
			cookie_.set_value(root["cookie"]["value"]);
			cookie_.set_comment(root["cookie"]["comment"]);
			cookie_.set_domain(root["cookie"]["domain"]);
			cookie_.set_path(root["cookie"]["path"]);
			cookie_.set_priority(root["cookie"]["priority"]);
			cookie_.set_secure(root["cookie"]["secure"]);
			cookie_.set_max_age(root["cookie"]["max_age"]);
			cookie_.set_http_only(root["cookie"]["http_only"]);
			is_update_ = true;
		}

	private:

		std::string id_;
		std::size_t expire_;
		std::time_t time_stamp_;
//		std::map<std::string, std::any> data_;
	    nlohmann::json data_;
		std::mutex mtx_;
		cookie cookie_;
		bool is_update_ = true;
	};
}