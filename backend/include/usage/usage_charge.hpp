#pragma once

#include <string_view>

#include "request/request.hpp"
#include "store/mysql.hpp"
#include "usage/usage_commit_jobs.hpp"

namespace revlm
{

void absorb_usage_object(Request &req, std::string_view body);

void fill_payload_from_request(UsageCommitPayload &payload, const Request &req);

void charge_request(MysqlConnection &conn, Request &billing_request, UsageCommitPayload &payload);

} // namespace revlm
