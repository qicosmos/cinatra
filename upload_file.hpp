#pragma once
#include <fstream>
#include <string>

namespace cinatra {
	class upload_file {
	public:
		void write(const char* data, size_t size) {
			file_size_ += size;
			file_.write(data, size);
		}

		bool open(const std::string& file_name) {
			file_.open(file_name, std::ios::binary);
			bool r = file_.is_open();
			if(r)
				file_path_ = std::string(&file_name[1], file_name.length()-1);

			return r;
		}

		void close() {
			file_.close();
		}

		size_t get_file_size() const{
			return file_size_;
		}

		std::string get_file_path() const{
			return file_path_;
		}
	private:
		//std::string file_name_;
		std::string file_path_;
		std::ofstream file_;
		size_t file_size_ = 0;
	};
}