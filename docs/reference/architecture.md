# 架构

本页记录当前 C++ 代码里仍然成立的结构。

## 组成

- **API 网关**：单个 `revlm` 进程。启动时应用 schema、加载插件包并注册它们的能力，然后提供健康检查、控制面、数据面代理与计费。
- **Web 静态前端**：`frontend/` 目录下的 Vite/React 应用，构建产物是 `frontend/dist`，独立托管。
- **MySQL**：系统状态的单一数据库来源；空库基线由 ODB 实体注解生成，之后的变更走 `backend/migrations/` 版本化 SQL，启动时由 `ensure_schema` 应用。
- **Redis（可选）**：用于跨实例并发限制、缓存失效协调和运行时共享状态。

## 代码边界

C++ 源码按领域拆在 `backend/src/<module>/`，头文件对应在 `backend/include/<module>/`，测试在 `backend/tests/<module>/`。

- `backend/src/main.cpp`：配置加载、schema、信号与 drain 生命周期。没有第二个可执行文件——bootstrap/worker 的拆分只是为了在 `exec` 前拼出 `LD_PRELOAD` 列表，插件现在由同一个进程 `dlopen`。
- `plugins/`：包扫描、解压、迁移、启用状态，以及路由表和模型目录这两张注册表（`registry.cpp`）。
- `server/`：HTTP 解析、核心路由注册（`http_dispatch.cpp`）、监听与 drain（`http_server.cpp`）。核心路由先注册，然后是插件，最后是无前缀的 catch-all——`httplib` 按注册顺序匹配，这个顺序是有意义的。
- `proxy/`：候选 Channel 轮转循环（`protocol_dispatch.cpp`）、上游传输与 SSRF 校验（`upstream.cpp`）、供插件调用的传输辅助（`transport.cpp`）。
- `channels/`：Channel 与 ChannelGroup 的 admin/API handler 与领域 store。
- `request/`：请求记录提交（`commit.cpp`）、`RequestStore`（ODB）与同步写入 `requests` / `request_totals`。
- `store/`：ODB 连接工厂（`database.cpp`）与 schema 应用（`schema.cpp` + `backend/migrations/`）。

## 运行模型

API 网关启动时：

1. 连接 MySQL 并 `ensure_schema`（需要 `REVLM_DB_DSN`）。
2. 注册核心路由，然后 `dlopen` 每个启用插件、执行它的迁移并调用它的注册入口；插件把数据面路由、模型目录和普通全局端点登记到核心注册表。
3. 注册无前缀 catch-all，开始服务：系统探针、`/api/*` 控制面，以及任何被插件认领的数据面路径。

插件不覆盖核心符号，也不依赖动态链接器的符号优先级；核心只按 `(方法, 路径, ChannelGroup.type)` 查表。

前端静态资源不由网关进程提供；由反向代理或静态托管直接服务 `frontend/dist`。

## 当前关键链路

1. 请求进入 `HttpServer`。
2. 系统探针直接返回；`api` 路径进入对应 handler 与 store；数据面请求先按用户 API key 解析出 ChannelGroup，再用方法、路径和该组的 type 查路由表，调用唯一命中的协议处理器。
3. 核心持有候选 Channel 的轮转循环，处理器每次返回一个尝试裁决；循环的唯一出口提交一次核心请求记录并扣费（交接过流的请求把这次提交推迟到 pump 结束）。
4. 需要 DB 的路径通过 MySQL 作为唯一状态来源；ODB 基线 + 版本化 SQL 保证表结构与实体/迁移一致。

## 数据合同

- 当前数据库合同以 [data-model.md](./data-model.md) 为准。
- 已经删除的订阅、配置化模型、旧 OAuth app、旧 session、旧 request rewrite 字段不再属于产品状态。
- 模型目录不是数据库表；由插件在注册阶段按 `ChannelGroup.type` 登记，核心只索引和转发，不解析价格 JSON。

## 相关页

- [API 手册](./api.md)
- [数据模型](./data-model.md)
- [C++ HTTP 边界](./router-boundaries.md)
