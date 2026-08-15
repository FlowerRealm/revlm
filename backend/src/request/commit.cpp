#include "proxy/protocol_dispatch.hpp"

#include "request/request.hpp"
#include "store/database.hpp"
#include "users/users.hpp"
#include "util/json.hpp"

namespace revlm
{

/*
 * The single exit of the candidate-rotation loop (CONTEXT 请求提交). Applies the
 * ChannelGroup multiplier, debits the balance and persists the request row in one
 * transaction -- reusing Request::commit for the persist + apply_total mechanics,
 * the way commit_proxy_usage in gateway.cpp used to, minus the token-derived
 * pricing that lived there.
 *
 * A request with no usage, or cut short, carries protocol_cost_usd == 0, so usd
 * comes out 0 too: debit_user_balance_usd() is a no-op for a non-positive delta,
 * and the captured status/latency/error still get persisted below.
 */
bool commit_proxy_request(ProxyRequest &proxy)
{
    // A request record means "this reached a channel". A request that never got
    // that far -- no active channel, no route -- leaves no row, which is what the
    // old commit_proxy_usage did and what every usage aggregate already assumes.
    if (proxy.id <= 0 || proxy.user_id <= 0 || proxy.token_id <= 0 || proxy.channel_id <= 0) {
        return false;
    }

    proxy.usd = proxy.protocol_cost_usd * proxy.channel_group_multiplier;

    Request req;
    req.id = proxy.id;
    req.time = proxy.time;
    req.user_id = proxy.user_id;
    req.request_id = proxy.request_id;
    req.response_id = proxy.response_id;
    req.endpoint = proxy.path;
    req.method = proxy.method;
    req.token_id = proxy.token_id;
    req.channel_group_multiplier = proxy.channel_group_multiplier;
    req.channel_id = proxy.channel_id;
    req.status_code = proxy.status_code;
    req.latency_ms = proxy.latency_ms;
    req.first_token_latency_ms = proxy.first_token_latency_ms;
    req.model_name = proxy.model_name;
    req.usage_details = serialize(proxy.usage_details);
    if (!proxy.error_message.empty()) {
        req.error_message = proxy.error_message;
    }
    req.usd = proxy.usd;

    odb::database &db = database();
    ScopedTransaction t(db);
    if (!UserStore::instance().debit_user_balance_usd(proxy.user_id, proxy.usd)) {
        return false;
    }
    if (!req.commit(proxy.time)) {
        return false;
    }
    t.commit();
    return true;
}

} // namespace revlm
