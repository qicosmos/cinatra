#pragma once
#include <map>
#include <any>
#include <mutex>
#include "uuid.h"

namespace cinatra {
	class session {
	public:
		template<typename T>
		void set(const std::string& name, T&& val) {
			std::unique_lock<std::mutex> lock(mtx_);
			last_used_time_ = std::time(nullptr);
			data_[name] = std::forward<T>(val);
		}

		bool has(const std::string& key){
			std::unique_lock<std::mutex> lock(mtx_);
			last_used_time_ = std::time(nullptr);
			return data_.find(key) != data_.end();
		}

		template<typename T>
		T& get(const std::string& name){
			std::unique_lock<std::mutex> lock(mtx_);
			last_used_time_ = std::time(nullptr);
			auto it = data_.find(name);
			if (it != data_.end())
			{
				return std::any_cast<T&>(it->second);
			}
			return {};
		}

		void del(const std::string& key){
			std::unique_lock<std::mutex> lock(mtx_);
			last_used_time_ = std::time(nullptr);
			auto it = data_.find(key);
			if (it == data_.end())
			{
				throw std::invalid_argument("key \"" + key + "\" not found.");
			}

			data_.erase(it);
		}

		std::time_t last_used_time() {
			return last_used_time_;
		}

	private:	
		std::string name_;
		std::string id_;
		std::string path_;
		std::string domain_;
		std::size_t expire_;
		std::time_t last_used_time_;
		std::mutex mtx_;
		std::map<std::string, std::any> data_;
	};

	class session_manager {
	public:
		static session_manager& get(){
			static session_manager instance;
			return instance;
		}

		std::string new_session(){
			uuids::uuid u = uuids::uuid_random_generator{}();
			std::string uid = uuids::to_string(u);

			{
				std::unique_lock<std::mutex> lock(mtx_);
				sessions_.emplace(uid, std::make_shared<session>());
			}

			return uid;
		}

		std::shared_ptr<session> get_session(const std::string& name) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto it = sessions_.find(name);
			return it == sessions_.end() ? nullptr : it->second;
		}

		void set_max_inactive_interval(int seconds) {
			max_inactive_interval_ = seconds;
		}

		void remove_expire() {
			std::unique_lock<std::mutex> lock(mtx_);
			for (auto it = sessions_.begin(); it != sessions_.end();){
				if (std::time(nullptr) - it->second->last_used_time() >= max_inactive_interval_){
					it = sessions_.erase(it);
				}
				else{
					++it;
				}
			}
		}

	private:
		session_manager() {};
		session_manager(const session_manager&) = delete;
		session_manager(session_manager&&) = delete;

		std::mutex mtx_;
		std::unordered_map<std::string, std::shared_ptr<session>> sessions_;
		int max_inactive_interval_ = 10 * 60;
	};
}