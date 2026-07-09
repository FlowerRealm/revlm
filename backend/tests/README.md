# C++ test scope

Committed tests cover **core semantics** and **one product chain** per AGENTS.md. Performance checks live under `scripts/perf/` and are not part of `ctest`.

## Product chains (CI-runnable)

| Chain | Primary targets | What it proves |
|-------|-----------------|----------------|
| Auth / session / token | `revlm_auth_middleware_test`, `revlm_auth_middleware_mysql_test`, `revlm_user_api_mysql_test`, `revlm_token_api_mysql_test`, `revlm_token_channel_groups_mysql_test`, `revlm_admin_users_test` | Register/login/logout, session binding, token CRUD/bindings, admin user mutations |
| Billing | `revlm_billing_test`, `revlm_billing_mysql_test` | Balance read/debit, admin manual top-up |
| Data-plane billing / usage | `revlm_quota_test`, `revlm_quota_mysql_test`, `revlm_http_server_mysql_*` gateway tests, `revlm_usage_test`, `revlm_usage_mysql_test` | PayGO admission/debit, sync usage event commit |
| Usage aggregation query | `revlm_usage_aggregation_mysql_test`, `revlm_http_server_tz_mysql_test`, `revlm_admin_usage_mysql_test` | Rollup coverage, window splitting, admin/user usage APIs |

## Supporting unit tests

Scheduler, proxy_request/proxy_response SSRF, runtime workers/cache, migrations, app settings, channel groups, stream pump, gateway resilience — each guards a single subsystem without duplicating the product chains above.

## Intentionally omitted

- Compatibility-only regressions (removed product behavior)
- Large perf / million-row suites (see `scripts/perf/`)
- Browser/e2e flows (manual temp-stack proof in Round 12 V003)

MySQL tests auto-skip when `REVLM_TEST_MYSQL_DSN` is unset unless they spawn a `tmp-revlm-*` container themselves.
