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
		string
	};

	inline std::string_view STAIC_RES = "cinatra_staic_resource";
}
