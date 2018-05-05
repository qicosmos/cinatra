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

	inline std::string_view STAIC_RES = "cinatra_staic_resource";
}
