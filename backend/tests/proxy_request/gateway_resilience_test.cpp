#include "proxy_request/gateway_resilience.hpp"
#include <iostream>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main()
{
    {
        revlm::GatewayRetryBudget budget({ .max_attempts = 3, .max_switches = 1, .max_elapsed_ms = 1000 });
        if (expect(budget.can_attempt(false), "initial attempt should be allowed") != 0) {
            return 1;
        }
        budget.note_attempt(false);
        if (expect(budget.can_attempt(true), "first switch should be allowed") != 0) {
            return 1;
        }
        budget.note_attempt(true);
        if (expect(!budget.can_attempt(true), "switch budget should stop second switch") != 0) {
            return 1;
        }
        if (expect(budget.can_attempt(false), "same candidate retry should still fit attempt budget") != 0) {
            return 1;
        }
        budget.note_attempt(false);
        if (expect(!budget.can_attempt(false), "attempt budget should stop fourth try") != 0) {
            return 1;
        }
    }

    {
        revlm::GatewayRetryBudget budget({ .max_attempts = 2, .max_switches = 1, .max_elapsed_ms = 5 });
        std::this_thread::sleep_for(10ms);
        if (expect(!budget.can_attempt(false), "elapsed budget should stop stale retry budget") != 0) {
            return 1;
        }
    }

    {
        const auto too_many = revlm::classify_gateway_status_failure(429);
        if (expect(too_many.retriable, "429 should be retriable") != 0 ||
            expect(too_many.failure_scope == revlm::SchedulerFailureScope::credential,
                   "429 should cool credential scope") != 0) {
            return 1;
        }

        const auto bad_request = revlm::classify_gateway_status_failure(400);
        if (expect(!bad_request.retriable, "400 should not retry") != 0 ||
            expect(bad_request.preserve_upstream_response, "400 should preserve upstream response") != 0) {
            return 1;
        }

        const auto wrong_route = revlm::classify_gateway_status_failure(404);
        if (expect(wrong_route.retriable, "404 should rotate to another channel") != 0 ||
            expect(wrong_route.failure_scope == revlm::SchedulerFailureScope::channel,
                   "404 should punish the channel scope") != 0) {
            return 1;
        }
    }

    {
        const auto network = revlm::classify_gateway_transport_failure("connect");
        if (expect(network.retriable, "network failure should retry") != 0 ||
            expect(network.error_class == "connect_upstream", "connect failure should classify as connect_upstream") !=
                0) {
            return 1;
        }

        const auto invalid_upstream = revlm::classify_gateway_transport_failure("parse");
        if (expect(invalid_upstream.retriable, "invalid upstream should still be retriable") != 0 ||
            expect(invalid_upstream.error_class == "invalid_upstream_url",
                   "parse failure should classify as invalid_upstream_url") != 0 ||
            expect(invalid_upstream.failure_scope == revlm::SchedulerFailureScope::channel,
                   "parse failure should cool endpoint scope") != 0) {
            return 1;
        }

        revlm::GatewayStreamPump pump;
        pump.idle_timeout = true;
        const auto stream = revlm::classify_gateway_stream_failure(pump, 200);
        if (expect(stream.retriable, "idle stream timeout should retry") != 0 ||
            expect(stream.error_class == "stream_idle_timeout", "idle stream timeout should keep stable class") != 0) {
            return 1;
        }
    }

    {
        std::vector<revlm::GatewayFailure> failures;
        failures.push_back(revlm::classify_gateway_transport_failure("connect"));
        failures.push_back(revlm::classify_gateway_status_failure(503));
        failures.push_back(revlm::classify_gateway_status_failure(400));
        if (expect(revlm::best_gateway_failure_index(failures) == 2,
                   "best failure should prefer preserved non-retriable upstream response") != 0) {
            return 1;
        }

        const auto scheduler_result = revlm::gateway_failure_to_scheduler_result(failures[1]);
        if (expect(!scheduler_result.success, "scheduler failure result should stay unsuccessful") != 0 ||
            expect(scheduler_result.retriable, "scheduler result should keep retriable bit") != 0 ||
            expect(scheduler_result.error_class == "upstream_status", "scheduler result should keep error class") !=
                0) {
            return 1;
        }
    }

    {
        std::vector<revlm::GatewayFailure> failures;
        failures.push_back(revlm::classify_gateway_status_failure(429));
        failures.push_back(revlm::classify_gateway_status_failure(400));
        if (expect(revlm::best_gateway_failure_index(failures) == 1,
                   "preserved upstream client error should beat retriable rate limit when all fail") != 0 ||
            expect(failures[1].preserve_upstream_response,
                   "400 should stay eligible for preserved upstream response") != 0) {
            return 1;
        }
    }

    return 0;
}
