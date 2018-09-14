#pragma once
#include <string>
#include <string_view>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "use_asio.hpp"
#include "response_cv.hpp"
#include <tuple>
namespace cinatra {
	constexpr const size_t MAX_CACHE_SIZE = 100000;
	using response_header_type = std::vector<std::pair<std::string, std::string>>;
	using cache_context_type = std::tuple<transfer_encoding_type,status_type,response_header_type,std::string>;
	class http_cache {
	public:
		static void add(const std::string& key, const cache_context_type& content) {
			std::unique_lock<std::mutex> lock(mtx_);
			
			if (std::distance(cur_it_, cache_.end()) > MAX_CACHE_SIZE) {
				cur_it_ = cache_.begin();
			}

			cur_it_ = cache_.emplace(key, content).first;
			cache_time_[key] = std::time(nullptr) + max_cache_age_;
		}

		static cache_context_type get(const std::string& key) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto time_it = cache_time_.find(key);
			auto it = cache_.find(key);
			auto now_time = std::time(nullptr);
			if(time_it != cache_time_.end() && time_it->second >= now_time){
				return it == cache_.end() ? cache_context_type{} : it->second;
			}else{
				if(time_it != cache_time_.end() && it != cache_.end()){
					cur_it_ = cache_.erase(it);
				}
				return cache_context_type{};
			}
		}

		static bool empty() {
			return cache_.empty();
		}

		static void update(const std::string& key) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto it = cache_.find(key);
			if (it != cache_.end())
				cache_.erase(it);
		}

		static void add_skip(std::string_view key) {
			skip_cache_.emplace(key);
		}

		static void add_single_cache(std::string_view key)
		{
			need_single_cache_.emplace(key);
		}

		static void enable_cache(bool b) {
			need_cache_ = b;
		}

		static bool need_cache(std::string_view key) {
			if(need_cache_){
				return need_cache_;
			}else{
                return need_single_cache_.find(key)!= need_single_cache_.end();
			}
		}

		static bool not_cache(std::string_view key) {
			return skip_cache_.find(key) != skip_cache_.end();
		}

		static void set_cache_max_age(std::time_t seconds)
		{
			max_cache_age_ = seconds;
		}

		static std::time_t get_cache_max_age()
		{
			return max_cache_age_;
		}

	private:
		static std::mutex mtx_;
		static bool need_cache_;
		static std::unordered_map<std::string, cache_context_type> cache_;
		static std::unordered_map<std::string, cache_context_type>::iterator cur_it_;
		static std::unordered_set<std::string_view> skip_cache_;
		static std::unordered_set<std::string_view> need_single_cache_;
		static std::time_t max_cache_age_;
		static std::unordered_map<std::string, std::time_t > cache_time_;
	};

	std::unordered_map<std::string, cache_context_type> http_cache::cache_;
	std::unordered_map<std::string, cache_context_type>::iterator http_cache::cur_it_= http_cache::cache_.begin();
	bool http_cache::need_cache_ = false;
	std::mutex http_cache::mtx_;
	std::unordered_set<std::string_view> http_cache::skip_cache_;
	std::time_t http_cache::max_cache_age_ = 0;
	std::unordered_map<std::string, std::time_t > http_cache::cache_time_;
	std::unordered_set<std::string_view> http_cache::need_single_cache_;
}