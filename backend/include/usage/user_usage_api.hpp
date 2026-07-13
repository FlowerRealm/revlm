#pragma once

#include "config/config.hpp"
#include "server/http_server.hpp"

#include <string_view>

namespace revlm
{

HttpResponse user_models_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id);
HttpResponse dashboard_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                     std::string_view target);
HttpResponse usage_windows_http_response(std::string_view raw_request, const Config &config,
                                         std::string_view request_id, std::string_view target);
HttpResponse requests_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                        std::string_view target);
HttpResponse usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target);
HttpResponse usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id, long long event_id);

} // namespace revlm
