#pragma once

#include "config/config.hpp"
#include "server/http_server.hpp"

#include <string_view>

namespace revlm
{

HttpResponse admin_dashboard_http_response(std::string_view raw_request, const Config &config,
                                           std::string_view request_id);
HttpResponse admin_usage_page_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target);
HttpResponse admin_usage_users_suggest_http_response(std::string_view raw_request, const Config &config,
                                                     std::string_view request_id, std::string_view target);
HttpResponse admin_usage_channels_suggest_http_response(std::string_view raw_request, const Config &config,
                                                        std::string_view request_id, std::string_view target);
HttpResponse admin_usage_models_suggest_http_response(std::string_view raw_request, const Config &config,
                                                      std::string_view request_id, std::string_view target);
HttpResponse admin_usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                                    std::string_view request_id, long long event_id);
HttpResponse admin_usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                                  std::string_view request_id, std::string_view target);

} // namespace revlm
