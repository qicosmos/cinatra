#pragma once
#include <string>
#include "utils.hpp"

namespace cinatra {
	class cookie {
	public:
		cookie() = default;
		cookie(const std::string& name, const std::string& value) : name_(name), value_(value) {

		}

		void set_version(int version) {
			is_need_update_ = true;
			version_ = version;
		}

		int get_version()
		{
			return version_;
		}

		void set_name(const std::string& name) {
			is_need_update_ = true;
			name_ = name;
		}

		std::string get_name() const {
			return name_;
		}

		void set_value(const std::string& value) {
			is_need_update_ = true;
			value_ = value;
		}

		std::string get_value() const {
			return value_;
		}

		void set_comment(const std::string& comment) {
			is_need_update_ = true;
			comment_ = comment;
		}

		std::string get_comment()
		{
			return comment_;
		}

		void set_domain(const std::string& domain) {
			is_need_update_ = true;
			domain_ = domain;
		}

		std::string get_domain() {
			return domain_ ;
		}

		void set_path(const std::string& path) {
			is_need_update_ = true;
			path_ = path;
		}

		std::string get_path() {
			return path_;
		}

		void set_priority(const std::string& priority) {
			is_need_update_ = true;
			priority_ = priority;
		}

		std::string get_priority() {
			return priority_;
		}

		void set_secure(bool secure) {
			is_need_update_ = true;
			secure_ = secure;
		}

		bool get_secure() {
			return secure_;
		}

		void set_max_age(std::time_t seconds) {
			is_need_update_ = true;
			max_age_ = seconds;
		}

		std::time_t get_max_age() {
			return max_age_;
		}

		void set_http_only(bool http_only) {
			is_need_update_ = true;
			http_only_ = http_only;
		}

		bool get_http_only() {
			return http_only_;
		}

		void set_need_update(bool flag)
		{
			is_need_update_ = flag;
		}

		bool is_need_update()
		{
		   return is_need_update_;
		}

		std::string to_string() const {
			std::string result;
			result.reserve(256);
			result.append(name_);
			result.append("=");
			if (version_ == 0)
			{
				// Netscape cookie
				result.append(value_);
				if (!path_.empty())
				{
					result.append("; path=");
					result.append(path_);
				}
				if (!priority_.empty())
				{
					result.append("; Priority=");
					result.append(priority_);
				}
				if (max_age_ != -1)
				{
					result.append("; expires=");
					result.append(get_gmt_time_str(max_age_));
				}
				if (secure_)
				{
					result.append("; secure");
				}
				if (http_only_)
				{
					result.append("; HttpOnly");
				}
			}
			else
			{
				// RFC 2109 cookie
				result.append("\"");
				result.append(value_);
				result.append("\"");
				if (!comment_.empty())
				{
					result.append("; Comment=\"");
					result.append(comment_);
					result.append("\"");
				}
				if (!path_.empty())
				{
					result.append("; Path=\"");
					result.append(path_);
					result.append("\"");
				}
				if (!priority_.empty())
				{
					result.append("; Priority=\"");
					result.append(priority_);
					result.append("\"");
				}

				if (max_age_ != -1)
				{
					result.append("; Max-Age=\"");
					result.append(std::to_string(max_age_));
					result.append("\"");
				}
				if (secure_)
				{
					result.append("; secure");
				}
				if (http_only_)
				{
					result.append("; HttpOnly");
				}
				result.append("; Version=\"1\"");
			}
			return result;
		}

	private:
		int          version_ = 0;
		std::string  name_ = "";
		std::string  value_ = "";
		std::string  comment_ = "";
		std::string  domain_ = "";
		std::string  path_ = "";
		std::string  priority_ = "";
		bool         secure_ = false;
		std::time_t  max_age_ = -1;
		bool         http_only_ = false;
		bool         is_need_update_ = false;
	};
}
