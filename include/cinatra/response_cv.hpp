#pragma once
#include "use_asio.hpp"
#include <string_view>

namespace cinatra {
	enum class status_type {
		init,
		switching_protocols = 101,
		ok = 200,
		created = 201,
		accepted = 202,
		no_content = 204,
		partial_content = 206,
		multiple_choices = 300,
		moved_permanently = 301,
		moved_temporarily = 302,
		not_modified = 304,
		bad_request = 400,
		unauthorized = 401,
		forbidden = 403,
		not_found = 404,
		internal_server_error = 500,
		not_implemented = 501,
		bad_gateway = 502,
		service_unavailable = 503
	};

	inline std::string_view ok = "OK";
	inline std::string_view created = "<html>"
		"<head><title>Created</title></head>"
		"<body><h1>201 Created</h1></body>"
		"</html>";

	inline std::string_view accepted =
		"<html>"
		"<head><title>Accepted</title></head>"
		"<body><h1>202 Accepted</h1></body>"
		"</html>";

	inline std::string_view no_content =
		"<html>"
		"<head><title>No Content</title></head>"
		"<body><h1>204 Content</h1></body>"
		"</html>";

	inline std::string_view multiple_choices =
		"<html>"
		"<head><title>Multiple Choices</title></head>"
		"<body><h1>300 Multiple Choices</h1></body>"
		"</html>";

	inline std::string_view moved_permanently =
		"<html>"
		"<head><title>Moved Permanently</title></head>"
		"<body><h1>301 Moved Permanently</h1></body>"
		"</html>";

	inline std::string_view moved_temporarily =
		"<html>"
		"<head><title>Moved Temporarily</title></head>"
		"<body><h1>302 Moved Temporarily</h1></body>"
		"</html>";

	inline std::string_view not_modified =
		"<html>"
		"<head><title>Not Modified</title></head>"
		"<body><h1>304 Not Modified</h1></body>"
		"</html>";

	inline std::string_view bad_request =
		"<html>"
		"<head><title>Bad Request</title></head>"
		"<body><h1>400 Bad Request</h1></body>"
		"</html>";

	inline std::string_view unauthorized =
		"<html>"
		"<head><title>Unauthorized</title></head>"
		"<body><h1>401 Unauthorized</h1></body>"
		"</html>";

	inline std::string_view forbidden =
		"<html>"
		"<head><title>Forbidden</title></head>"
		"<body><h1>403 Forbidden</h1></body>"
		"</html>";

	inline std::string_view not_found =
		"<html>"
		"<head><title>Not Found</title></head>"
		"<body><h1>404 Not Found</h1></body>"
		"</html>";

	inline std::string_view internal_server_error =
		"<html>"
		"<head><title>Internal Server Error</title></head>"
		"<body><h1>500 Internal Server Error</h1></body>"
		"</html>";

	inline std::string_view not_implemented =
		"<html>"
		"<head><title>Not Implemented</title></head>"
		"<body><h1>501 Not Implemented</h1></body>"
		"</html>";

	inline std::string_view bad_gateway =
		"<html>"
		"<head><title>Bad Gateway</title></head>"
		"<body><h1>502 Bad Gateway</h1></body>"
		"</html>";

	inline std::string_view service_unavailable =
		"<html>"
		"<head><title>Service Unavailable</title></head>"
		"<body><h1>503 Service Unavailable</h1></body>"
		"</html>";

	inline std::string_view switching_protocols = "HTTP/1.1 101 Switching Protocals\r\n";
	inline std::string_view rep_ok = "HTTP/1.1 200 OK\r\n";
	inline std::string_view rep_created = "HTTP/1.1 201 Created\r\n";
	inline std::string_view rep_accepted = "HTTP/1.1 202 Accepted\r\n";
	inline std::string_view rep_no_content = "HTTP/1.1 204 No Content\r\n";
	inline std::string_view rep_partial_content = "HTTP/1.1 206 Partial Content\r\n";
	inline std::string_view rep_multiple_choices = "HTTP/1.1 300 Multiple Choices\r\n";
	inline std::string_view rep_moved_permanently =	"HTTP/1.1 301 Moved Permanently\r\n";
	inline std::string_view rep_moved_temporarily =	"HTTP/1.1 302 Moved Temporarily\r\n";
	inline std::string_view rep_not_modified = "HTTP/1.1 304 Not Modified\r\n";
	inline std::string_view rep_bad_request = "HTTP/1.1 400 Bad Request\r\n";
	inline std::string_view rep_unauthorized = "HTTP/1.1 401 Unauthorized\r\n";
	inline std::string_view rep_forbidden =	"HTTP/1.1 403 Forbidden\r\n";
	inline std::string_view rep_not_found =	"HTTP/1.1 404 Not Found\r\n";
	inline std::string_view rep_internal_server_error = "HTTP/1.1 500 Internal Server Error\r\n";
	inline std::string_view rep_not_implemented = "HTTP/1.1 501 Not Implemented\r\n";
	inline std::string_view rep_bad_gateway = "HTTP/1.1 502 Bad Gateway\r\n";
	inline std::string_view rep_service_unavailable = "HTTP/1.1 503 Service Unavailable\r\n";

	inline const char name_value_separator[] = { ':', ' ' };
	//inline std::string_view crlf = "\r\n";

	inline const char crlf[] = { '\r', '\n' };
	inline const char last_chunk[] = { '0', '\r', '\n' };
	inline const std::string http_chunk_header =
		"HTTP/1.1 200 OK\r\n"
		"Transfer-Encoding: chunked\r\n";
		/*"Content-Type: video/mp4\r\n"
		"\r\n";*/

	boost::asio::const_buffer to_buffer(status_type status) {
		switch (status) {
		case status_type::switching_protocols:
			return boost::asio::buffer(switching_protocols.data(), switching_protocols.length());
		case status_type::ok:
			return boost::asio::buffer(rep_ok.data(), rep_ok.length());
		case status_type::created:
			return boost::asio::buffer(rep_created.data(), rep_created.length());
		case status_type::accepted:
			return boost::asio::buffer(rep_accepted.data(), rep_created.length());
		case status_type::no_content:
			return boost::asio::buffer(rep_no_content.data(), rep_no_content.length());
		case status_type::partial_content:
			return boost::asio::buffer(rep_partial_content.data(), rep_partial_content.length());
		case status_type::multiple_choices:
			return boost::asio::buffer(rep_multiple_choices.data(), rep_multiple_choices.length());
		case status_type::moved_permanently:
			return boost::asio::buffer(rep_moved_permanently.data(), rep_moved_permanently.length());
		case status_type::moved_temporarily:
			return boost::asio::buffer(rep_moved_temporarily.data(), rep_moved_temporarily.length());
		case status_type::not_modified:
			return boost::asio::buffer(rep_not_modified.data(), rep_not_modified.length());
		case status_type::bad_request:
			return boost::asio::buffer(rep_bad_request.data(), rep_bad_request.length());
		case status_type::unauthorized:
			return boost::asio::buffer(rep_unauthorized.data(), rep_unauthorized.length());
		case status_type::forbidden:
			return boost::asio::buffer(rep_forbidden.data(), rep_forbidden.length());
		case status_type::not_found:
			return boost::asio::buffer(rep_not_found.data(), rep_not_found.length());
		case status_type::internal_server_error:
			return boost::asio::buffer(rep_internal_server_error.data(), rep_internal_server_error.length());
		case status_type::not_implemented:
			return boost::asio::buffer(rep_not_implemented.data(), rep_not_implemented.length());
		case status_type::bad_gateway:
			return boost::asio::buffer(rep_bad_gateway.data(), rep_bad_gateway.length());
		case status_type::service_unavailable:
			return boost::asio::buffer(rep_service_unavailable.data(), rep_service_unavailable.length());
		default:
			return boost::asio::buffer(rep_internal_server_error.data(), rep_internal_server_error.length());
		}
	}

	inline std::string_view to_string(status_type status) {
		switch (status) {
		case status_type::ok:
			return ok;
		case status_type::created:
			return created;
		case status_type::accepted:
			return accepted;
		case status_type::no_content:
			return no_content;
		case status_type::multiple_choices:
			return multiple_choices;
		case status_type::moved_permanently:
			return moved_permanently;
		case status_type::moved_temporarily:
			return moved_temporarily;
		case status_type::not_modified:
			return not_modified;
		case status_type::bad_request:
			return bad_request;
		case status_type::unauthorized:
			return unauthorized;
		case status_type::forbidden:
			return forbidden;
		case status_type::not_found:
			return not_found;
		case status_type::internal_server_error:
			return internal_server_error;
		case status_type::not_implemented:
			return not_implemented;
		case status_type::bad_gateway:
			return bad_gateway;
		case status_type::service_unavailable:
			return service_unavailable;
		default:
			return internal_server_error;
		}
	}
}

