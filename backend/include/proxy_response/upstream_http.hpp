#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "proxy_request/upstream.hpp"
#include "server/http_server.hpp"

namespace revlm
{

using ClientWriter = std::function<bool(std::string_view)>;

ClientWriter client_writer_from_fd(int fd);

bool is_sse_content_type(std::string_view content_type);

HttpResponse make_upstream_http_response(int status, std::string body, std::vector<Header> headers = {});

std::string drain_upstream_stream_body(UpstreamStreamResponse &upstream);

std::string format_upstream_proxy_response_headers(int status_code, const std::vector<UpstreamHeader> &headers,
                                                   size_t body_size);

std::string build_synthetic_stream_response_head(int status, std::string_view content_type,
                                                 const std::vector<Header> &headers = {});

std::string read_remaining_stream(const UpstreamReadHandle &stream);

} // namespace revlm
