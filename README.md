# Revlm

Revlm 后端已经切到单体 C++ 服务，按 `REVLM_NODE_ROLE=api|web|all` 运行。前端仍然是独立的 Vite/React 静态应用，默认构建为 `frontend/dist` 后单独托管；发布的 Docker/Helm 镜像只包含后端二进制和 SQL migrations，不再内置前端。

## 当前后端状态

- C++ 服务负责健康检查、基础元信息、用户会话、管理设置、支付渠道和充值链路。
- `api` / `all` role 启动时会自动执行 `internal/store/migrations/*.sql`。
- `web` role 负责静态文件和 SPA fallback；命中 `/api`、`/v1`、`/v1beta`、`/oauth`、`/auth/callback` 时反代到 `REVLM_PROXY_UPSTREAM_BASE_URL`。
- `web` role 必须配置 `REVLM_PROXY_UPSTREAM_BASE_URL`；`api` / `all` role 必须配置 `REVLM_DB_DSN` 和 `SESSION_SECRET`。

## 本地构建

```bash
sudo apt install cmake libssl-dev libcpp-httplib-dev libboost-json-dev libboost-url-dev
cmake -S . -B build/backend -DCMAKE_BUILD_TYPE=Release
cmake --build build/backend -j"$(nproc)"
```

提交前格式检查（C++ `clang-format`、前端 Prettier）：

```bash
sudo apt install pre-commit
pre-commit install
```

## 本地运行

只跑后端 API：

```bash
REVLM_NODE_ROLE=api \
REVLM_ADDR=127.0.0.1:8080 \
REVLM_DB_DSN='user:pass@tcp(127.0.0.1:3306)/revlm?parseTime=true&charset=utf8mb4&collation=utf8mb4_unicode_ci&time_zone=%27%2B00%3A00%27' \
SESSION_SECRET='replace-with-a-stable-random-secret' \
build/backend/revlm
```

只跑前端静态入口和 API 反代：

```bash
REVLM_NODE_ROLE=web \
REVLM_ADDR=127.0.0.1:8080 \
REVLM_WEB_STATIC_DIR=frontend/dist \
REVLM_PROXY_UPSTREAM_BASE_URL='http://127.0.0.1:8081' \
build/backend/revlm
```

同时跑静态入口与 API：

```bash
REVLM_NODE_ROLE=all \
REVLM_ADDR=127.0.0.1:8080 \
REVLM_DB_DSN='user:pass@tcp(127.0.0.1:3306)/revlm?parseTime=true&charset=utf8mb4&collation=utf8mb4_unicode_ci&time_zone=%27%2B00%3A00%27' \
SESSION_SECRET='replace-with-a-stable-random-secret' \
REVLM_WEB_STATIC_DIR=frontend/dist \
build/backend/revlm
```

## 当前 HTTP 面

系统探针与指标：

- `GET /healthz`、`/livez`、`/readyz`
- `GET /metrics`
- `GET /api/meta`

控制面覆盖用户会话、token、渠道/渠道组、用量查询、计费充值、管理设置与用户管理；数据面覆盖 `/v1/*` 与 `/v1beta/openai/models`。完整路由清单见 `docs/reference/api.md`。

## 前端开发

```bash
cd frontend
npm ci
npm run dev
```

Vite dev server 默认把后端路径代理到 `http://localhost:8080`。

## 前端构建

```bash
cd frontend
npm ci
npm run build
```

## 验证

```bash
cmake -S . -B build/backend -DCMAKE_BUILD_TYPE=Release
cmake --build build/backend -j"$(nproc)"
ctest --test-dir build/backend --output-on-failure
npm --prefix frontend run lint
npm --prefix frontend run fmt:check
npm --prefix frontend run build
```

`build/backend` 下的测试二进制会自动回退到源码根的 `internal/store/migrations`，因此可以直接从 `build/backend` 执行 `ctest`。

## Maintenance CLI

- `build/backend/tmp-devseed`: **仅用于本地开发**。它会执行 migrations，并确保本地库里存在固定的 `root@local` / `user@local` 两个账号（弱密码，见 `backend/src/tmp_devseed.cpp`）。禁止对生产库或公网可达实例运行。
- `backfill-usage-stats` / `backfill-daily-stats`: 当前刻意不提供。usage aggregation/backfill 还没有在这条 C++ 基线中落地，所以这里不做半成品替代。

本地 seed 示例：

```bash
REVLM_NODE_ROLE=api \
REVLM_DB_DSN='user:pass@tcp(127.0.0.1:3306)/revlm?parseTime=true&charset=utf8mb4&collation=utf8mb4_unicode_ci&time_zone=%27%2B00%3A00%27' \
SESSION_SECRET='replace-with-a-stable-random-secret' \
build/backend/tmp-devseed
```

## 关键环境变量

- `REVLM_ENV`
- `REVLM_ADDR`
- `REVLM_NODE_ROLE`
- `REVLM_WEB_STATIC_DIR`
- `REVLM_PROXY_UPSTREAM_BASE_URL`
- `REVLM_DB_DSN`
- `SESSION_SECRET`
- `REVLM_ADMIN_API_KEY`
- `REVLM_REDIS_ADDR`
- `REVLM_REDIS_PASSWORD`
- `REVLM_REDIS_DB`
- `REVLM_REDIS_KEY_PREFIX`
- `REVLM_COMPACT_GATEWAY_BASE_URL`
- `REVLM_COMPACT_GATEWAY_KEY`
- `REVLM_SHUTDOWN_GRACE_PERIOD_SECONDS`
- `REVLM_HTTP_READ_HEADER_TIMEOUT_SECONDS`
- `REVLM_HTTP_MAX_HEADER_BYTES`
- `REVLM_HTTP_MAX_BODY_BYTES`
- `REVLM_PROXY_UPSTREAM_TIMEOUT_SECONDS`
- `REVLM_DB_MAX_OPEN_CONNS`
- `REVLM_DB_MAX_IDLE_CONNS`
- `REVLM_DB_CONN_MAX_LIFETIME_SECONDS`
- `REVLM_DB_CONN_MAX_IDLE_TIME_SECONDS`
- `REVLM_DB_MIGRATION_LOCK_TIMEOUT_SECONDS`
- `REVLM_GATEWAY_MAX_RETRY_ATTEMPTS`
- `REVLM_GATEWAY_RETRY_BASE_DELAY_MS`
- `REVLM_GATEWAY_RETRY_MAX_DELAY_MS`
- `REVLM_GATEWAY_MAX_RETRY_ELAPSED_MS`
- `REVLM_GATEWAY_MAX_FAILOVER_SWITCHES`
- `REVLM_GATEWAY_CREDENTIAL_MAX_CONCURRENCY`
- `REVLM_GATEWAY_WAIT_TIMEOUT_MS`
- `REVLM_GATEWAY_WAIT_QUEUE_EXTRA_SLOTS`
- `REVLM_ROUTING_REFRESH_MS`
- `REVLM_ROUTING_REBUILD_DEBOUNCE_MS`
- `REVLM_AUTH_LOCAL_TTL_MS`
- `REVLM_AUTH_REDIS_TTL_MS`
- `REVLM_USAGE_FINALIZE_FLUSH_MS`
- `REVLM_USAGE_FINALIZE_BATCH_SIZE`
- `REVLM_USAGE_FINALIZE_QUEUE_SIZE`
- `REVLM_USAGE_FINALIZE_WORKERS`
- `REVLM_USAGE_COMMIT_POLL_MS`
- `REVLM_USAGE_COMMIT_CLAIM_SIZE`
- `REVLM_USAGE_COMMIT_WORKERS`
- `REVLM_USAGE_COMMIT_LEASE_MS`
- `REVLM_USAGE_COMMIT_STALE_MS`

## 文档

- 部署：`docs/deployment/overview.md`
- 参考：`docs/reference/api.md`、`docs/reference/architecture.md`、`docs/reference/data-model.md`
- 安全：`SECURITY.md`
