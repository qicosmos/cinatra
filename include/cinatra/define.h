#pragma once
namespace cinatra {
	enum class content_type {
		string,
		multipart,
		urlencoded,
		chunked,
		octet_stream,
		websocket,
		json,
		unknown,
	};

	enum class res_content_type{
		html,
		json,
		string,
        none
	};

	inline const std::string_view STAIC_RES = "cinatra_staic_resource";
	inline const std::string CSESSIONID = "CSESSIONID";
}
