#pragma once
namespace cinatra {
	enum class content_type {
		string,
		multipart,
		urlencoded,
		chunked,
		octet_stream,
		websocket,
		unknown,
	};

	enum class res_content_type{
		html,
		json,
		string,
        none
	};

	inline const std::string_view STATIC_RESOURCE = "cinatra_static_resource";
	inline const std::string CSESSIONID = "CSESSIONID";
}
