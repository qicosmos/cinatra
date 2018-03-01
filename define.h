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
}
