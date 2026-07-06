# C++ HTTP/角色边界

本页描述当前 C++ 后端的 HTTP 边界。

## 角色边界

`Config.role` 决定进程暴露什么能力：

| role | 能力 |
| --- | --- |
| `api` | 健康检查、`/api/*`、支付回调、数据库迁移 |
| `web` | 静态文件、SPA fallback、代理 `/api` `/v1` `/v1beta` `/oauth` `/auth/callback` |
| `all` | 同时提供 `api` 和 `web` |

约束：

- `api` / `all` 必须有 `REVLM_DB_DSN` 与 `SESSION_SECRET`。
- `web` 必须有 `REVLM_PROXY_UPSTREAM_BASE_URL`。
- 所有角色都会暴露 `/healthz`、`/livez`、`/readyz`。

## 当前请求分发

`backend/src/server/http_server.cpp` 的分发顺序是：

1. 先处理 `/healthz`、`/livez`、`/readyz`、`/metrics`
2. 如果是 `api` 能力，处理 `/api/*`、`/v1/*`、`/v1beta/*`、`/auth/callback` 与支付回调
3. 如果是纯 `web` 且命中代理路径，直接流式转发到 `REVLM_PROXY_UPSTREAM_BASE_URL`
4. 如果有 `web` 能力且不是代理路径，返回静态文件或 `index.html` fallback
5. 其他路径返回 404

这几个路径前缀被视为代理路径：

- `/api`
- `/v1`
- `/v1beta`
- `/oauth`
- `/auth/callback`

## 当前模块职责

- `main.cpp`：配置校验、build info、listener 生命周期、drain 信号。
- `http_server.cpp`：HTTP 解析、body/header 限制、系统探针、静态资源、代理、路由分发。
- `users.cpp` / `tokens.cpp`：用户会话、API token 与绑定。
- `channel_admin_api.cpp` / `channels.cpp`：渠道管理。
- `channel_groups_admin_api.cpp` / `channel_groups.cpp`：渠道组 CRUD 与成员调度。
- `user_usage_api.cpp` / `admin_usage_api.cpp` / `usage.cpp`：用量查询与聚合。
- `app_settings.cpp` / `billing.cpp`：设置与余额扣费。
- `proxy_request.cpp` / `proxy_response/` / `scheduler.cpp`：数据面代理与上游调度。
- `runtime_workers.cpp` / `usage_commit_jobs.cpp`：异步用量 worker。
- `migrations.cpp`：schema migration runner。

## 当前认证边界

- 浏览器登录后通过 `revlm_session` Cookie 访问用户接口。
- 当前管理接口走 root session 边界。
- `REVLM_ADMIN_API_KEY` 仍属于配置面的一部分，但当前 `http_server.cpp` 已实现的管理接口并没有把它作为主认证路径写进合同。

## 静态资源边界

- 静态根目录来自 `REVLM_WEB_STATIC_DIR`。
- `/assets/*` 不做 SPA fallback，缺失就返回 404。
- 不带扩展名的路径默认 fallback 到 `index.html`。
- `safe_static_path` 阻止 `..`、反斜杠和编码点路径穿越。

## 相关页

- [API 手册](./api.md)
- [架构](./architecture.md)
