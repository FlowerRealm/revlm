# 架构

本页记录当前 C++ 代码里仍然成立的结构。

## 组成

- **C++ Runtime**：单二进制后端，按 `api` / `web` / `all` 角色运行。
- **Web 静态前端**：`frontend/` 目录下的 Vite/React 应用，构建产物是 `frontend/dist`。
- **MySQL**：系统状态的单一数据库来源，schema 由 `internal/store/migrations/*.sql` 维护。
- **Redis（可选）**：用于跨实例并发限制、缓存失效协调和运行时共享状态。

## 代码边界

C++ 源码按领域拆在 `backend/src/<module>/`，头文件对应在 `backend/include/<module>/`，测试在 `backend/tests/<module>/`。

- `backend/src/main.cpp`：进程入口、配置加载、信号与 drain 生命周期。
- `server/`：HTTP 解析、路由分发、静态资源、反代、数据面代理入口（`http_server.cpp`）、token store（`tokens.cpp`）。
- `channels/`、`usage/`：按 HTTP 面拆分的 admin/API handler 与领域 store，例如 `channel_admin_api.cpp`、`user_usage_api.cpp`、`admin_usage_api.cpp`、`channels.cpp`、`usage.cpp`。
- `proxy_request/`、`proxy_response/`、`scheduler/`：数据面请求/响应代理、上游调度、failover 与并发控制。
- `runtime/`、`usage/`：异步用量 finalize 与 commit worker（`runtime_workers.cpp`、`usage_commit_jobs.cpp`）。
- `store/`：MySQL 连接与 schema migration runner（`mysql.cpp`、`migrations.cpp`）。
- `internal/store/migrations/*.sql`：数据库 schema 的事实来源。

## 当前运行模型

### `api`

- 暴露系统探针与 `/api/*` 接口。
- 启动时执行数据库迁移。
- 需要 `REVLM_DB_DSN` 与 `SESSION_SECRET`。

### `web`

- 只服务静态文件与 SPA fallback。
- 对 `/api`、`/v1`、`/v1beta`、`/oauth`、`/auth/callback` 做反代。
- 不连接数据库，但必须配置 `REVLM_PROXY_UPSTREAM_BASE_URL`。

### `all`

- 在同一个进程里同时承担 `api` 与 `web` 两种职责。

## 当前关键链路

1. 请求进入 `HttpServer`。
2. 系统探针直接返回；静态路径读 `REVLM_WEB_STATIC_DIR`；代理路径在 `web` role 下流式转发到上游 API。
3. `api` 路径进入对应 handler 与 store；数据面 `/v1/*` 走上游调度与用量 finalize/commit。
4. 需要 DB 的路径通过 MySQL 作为唯一状态来源；迁移 runner 保证 schema 达到当前合同。

## 数据合同

- 当前数据库合同以 [data-model.md](./data-model.md) 为准。
- 已经删除的订阅、配置化模型、旧 OAuth app、旧 session、旧 request rewrite 字段不再属于产品状态。
- 模型目录不是数据库表，而是后续 C++ 常量和运行时可达性计算的结果。

## 相关页

- [API 手册](./api.md)
- [数据模型](./data-model.md)
- [C++ HTTP/角色边界](./router-boundaries.md)
