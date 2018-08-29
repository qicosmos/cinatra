//
// Created by xmh on 18-5-7.
//

#ifndef CINATRA_SESSION_UTILS_HPP
#define CINATRA_SESSION_UTILS_HPP
#include "session.hpp"
#include "request.hpp"
#include <fstream>
#ifdef _MSC_VER
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

namespace cinatra {
	class session_manager {
	public:
		session_manager() = delete;
		static std::shared_ptr<session> create_session(const std::string& name, std::size_t expire, 
			const std::string& path = "/", const std::string& domain = ""){
			uuids::uuid_random_generator uid{};
			std::string uuid_str = uid().to_short_str();
			auto s = std::make_shared<session>(name, uuid_str, expire, path, domain);

			{
				std::unique_lock<std::mutex> lock(mtx_);
				map_.emplace(std::move(uuid_str), s);
			}
			write_session_to_file(s);
			return s;
		}

		static std::shared_ptr<session> create_session(std::string_view host, const std::string& name,
			std::time_t expire = -1, const std::string &path = "/") {
			auto pos = host.find(":");
			if (pos != std::string_view::npos){
				host = host.substr(0, pos);
			}

			return create_session(name, expire, path, std::string(host.data(), host.length()));
		}

		static std::weak_ptr<session> get_session(const std::string& id) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto it = map_.find(id);
			return (it != map_.end()) ? it->second : nullptr;
		}

		static void del_session(const std::string& id) {
			{
				std::unique_lock<std::mutex> lock(mtx_);
				auto it = map_.find(id);
				if (it != map_.end()){
					fs::remove(std::string("./")+session_db_directory+"/"+it->second->get_id());
					map_.erase(it);
				}
			}
			write_all_session_to_file();
		}

		static void check_expire() {
			if (map_.empty())
				return;

			auto now = std::time(nullptr);
			{
				std::unique_lock<std::mutex> lock(mtx_);
				for (auto it = map_.begin(); it != map_.end();) {
					if (now - it->second->time_stamp() >= max_age_) {
						fs::remove(std::string("./")+session_db_directory+"/"+it->second->get_id());
						it = map_.erase(it);
					}
					else {
						++it;
					}
				}
			}

			write_all_session_to_file();
		}

		static void write_session_to_file(std::shared_ptr<session> session)
		{
			std::unique_lock<std::mutex> lock(mtx_);
			std::string file_path = std::string("./")+session_db_directory+"/"+session->get_id();
			std::ofstream file(file_path,std::ios_base::out);
			if(file.is_open()){
				file << session->serialize_to_object();
			}
			file.close();
		}

		static void write_all_session_to_file()
		{
			std::unique_lock<std::mutex> lock(mtx_);
			for(auto iter = map_.begin();iter!=map_.end();++iter){
				std::string file_path = std::string("./")+session_db_directory+"/"+iter->second->get_id();
				std::ofstream file(file_path,std::ios_base::out);
				if(file.is_open()){
					file << iter->second->serialize_to_object();
				}
				file.close();
			}
		}

		static void read_all_session_from_file()
		{
			std::unique_lock<std::mutex> lock(mtx_);
			std::string session_path = std::string("./")+session_db_directory;
			for(auto& iter:fs::directory_iterator(session_path.data())){
				auto fp = iter.path();
				nlohmann::json json;
				std::ifstream file(session_path+"/"+fp.filename().c_str(),std::ios_base::in);
				if(file.is_open()){
					file >> json;
					auto s = std::make_shared<session>();
					s->serialize_from_object(json);
                    file.close();
					map_.emplace(s->get_id(), s);
				}
			}
		}

		static void set_max_inactive_interval(int seconds) {
			max_age_ = seconds;
		}

		static void set_session_db_directory(const std::string& name)
		{
			session_db_directory = name;
		}

	private:	
		static std::map<std::string, std::shared_ptr<session>> map_;
		static std::mutex mtx_;
		static int max_age_;
		static std::string session_db_directory;
	};
	std::map<std::string, std::shared_ptr<session>> session_manager::map_;
	std::mutex session_manager::mtx_;
	int session_manager::max_age_ = 0;
	std::string session_manager::session_db_directory;
}
#endif //CINATRA_SESSION_UTILS_HPP
