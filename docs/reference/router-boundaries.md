# C++ HTTP 边界

本页描述当前 C++ API 网关的 HTTP 边界。

## 能力范围

网关进程只提供 API，不提供静态前端：

- 健康检查：`/healthz`、`/livez`、`/readyz`
- 指标：`/metrics`
- 控制面：`/api/*`
- 数据面：`/v1/*`、`/v1beta/*`
- 支付回调：`/auth/callback`

约束：

- 必须有 `REVLM_DB_DSN` 与 `SESSION_SECRET`
- 始终暴露 `/healthz`、`/livez`、`/readyz`

## 请求分发

`backend/src/server/http_dispatch.cpp` 的分发顺序：

1. 处理 `/healthz`、`/livez`、`/readyz`、`/metrics`
2. 处理 `/api/*`、`/v1/*`、`/v1beta/*`、`/auth/callback` 与支付回调
3. 其他路径返回 404

## 模块职责

- `main.cpp`：配置校验、build info、listener 生命周期、drain 信号。
- `http_server.cpp`：HTTP 解析、body/header 限制、系统探针、路由分发。
- `users.cpp` / `tokens.cpp`：用户会话、API token 与绑定。
- `channel_admin_api.cpp` / `channels.cpp`：渠道管理。
- `channel_groups_admin_api.cpp` / `channel_groups.cpp`：渠道组 CRUD 与成员调度。
- `http_dispatch.cpp`：用量查询与聚合（用户/管理仪表盘、事件、时间序列）；`request.cpp` / `RequestStore` 负责 ODB 读写与计价。
- `app_settings.cpp` / `billing.cpp`：设置与余额扣费。
- `proxy_request/` / `proxy_response/` / `scheduler.cpp`：数据面代理与上游调度。
- `request.cpp`：请求计价与同步写入 `usage_events`。
- `database.cpp`：ODB 连接工厂与 SQL 助手。
- `schema.cpp` + `backend/migrations/`：空库 ODB 基线与版本化 SQL 迁移（`ensure_schema`）。

## 认证边界

- 浏览器登录后通过 `revlm_session` Cookie 访问用户接口。
- 当前管理接口走 root session 边界。
- `REVLM_ADMIN_API_KEY` 仍属于配置面的一部分，但当前 `http_server.cpp` 已实现的管理接口并没有把它作为主认证路径写进合同。

## 相关页

- [API 手册](./api.md)
- [架构](./architecture.md)
