# 架构

本页记录当前 C++ 代码里仍然成立的结构。

## 组成

- **API 网关**：C++ bootstrap + worker。bootstrap 负责 schema、插件 migration 和 Linux preload，
  worker 负责健康检查、控制面、数据面代理与计费。
- **Web 静态前端**：`frontend/` 目录下的 Vite/React 应用，构建产物是 `frontend/dist`，独立托管。
- **MySQL**：系统状态的单一数据库来源；空库基线由 ODB 实体注解生成，之后的变更走 `backend/migrations/` 版本化 SQL，启动时由 `ensure_schema` 应用。
- **Redis（可选）**：用于跨实例并发限制、缓存失效协调和运行时共享状态。

## 代码边界

C++ 源码按领域拆在 `backend/src/<module>/`，头文件对应在 `backend/include/<module>/`，测试在 `backend/tests/<module>/`。

- `backend/src/main.cpp`：短生命周期 bootstrap；解析包、设置 `LD_PRELOAD` 后 `exec` worker。
- `backend/src/main_worker.cpp`：配置加载、信号与 drain 生命周期。
- `plugins/`：包解压、迁移、启用状态和 preload 顺序；不维护任何插件能力 registry。
- `server/`：HTTP 解析、路由分发、数据面代理入口（`http_server.cpp`）、token store（`tokens.cpp`）。
- `channels/`：按 HTTP 面拆分的 admin/API handler 与领域 store，例如 `channel_api.cpp`、`channels.cpp`。
- `server/http_dispatch.cpp`：控制面路由与用量查询 handler（用户/管理仪表盘、事件列表、时间序列）。
- `proxy_request/`、`proxy_response/`、`scheduler/`：数据面请求/响应代理、上游调度、failover 与并发控制。
- `request/`：请求计价、`RequestStore`（ODB）与同步写入 `requests` / `request_totals`。
- `store/`：ODB 连接工厂（`database.cpp`）与 schema 应用（`schema.cpp` + `backend/migrations/`）。

## 运行模型

API 网关启动时：

1. bootstrap 连接 MySQL 并 `ensure_schema`（需要 `REVLM_DB_DSN`），执行启用插件 migration。
2. bootstrap 将模块按包依赖与 `load_order` 放入 `LD_PRELOAD`，`exec` worker。
3. worker 暴露系统探针、`/api/*` 控制面与 `/v1/*` 数据面；插件可覆盖任意可插桩的普通核心函数。

前端静态资源不由网关进程提供；由反向代理或静态托管直接服务 `frontend/dist`。

## 当前关键链路

1. 请求进入 `HttpServer`。
2. 系统探针直接返回；`api` 路径进入对应 handler 与 store；数据面 `/v1/*` 走上游调度，结束后同步扣费并写入 `usage_events`。
3. 需要 DB 的路径通过 MySQL 作为唯一状态来源；ODB 基线 + 版本化 SQL 保证表结构与实体/迁移一致。

## 数据合同

- 当前数据库合同以 [data-model.md](./data-model.md) 为准。
- 已经删除的订阅、配置化模型、旧 OAuth app、旧 session、旧 request rewrite 字段不再属于产品状态。
- 模型目录不是数据库表；当前由预加载协议模块提供，也可被任意插件的普通 C++ 替换重写。

## 相关页

- [API 手册](./api.md)
- [数据模型](./data-model.md)
- [C++ HTTP 边界](./router-boundaries.md)
