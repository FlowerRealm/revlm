# 架构

本页记录当前 C++ 代码里仍然成立的结构。

## 组成

- **API 网关**：C++ bootstrap + worker。bootstrap 负责核心 schema、包状态和不可变插件快照；worker 负责加载 v1 模块、健康检查、控制面、数据面代理与计费。
- **Web 静态前端**：`frontend/` 目录下的 Vite/React 应用，构建产物是 `frontend/dist`，独立托管。
- **MySQL**：系统状态的单一数据库来源；空库基线由 ODB 实体注解生成，之后的变更走 `backend/migrations/` 版本化 SQL，启动时由 `ensure_schema` 应用。
- **Redis（可选）**：用于跨实例并发限制、缓存失效协调和运行时共享状态。

## 代码边界

C++ 源码按领域拆在 `backend/src/<module>/`，头文件对应在 `backend/include/<module>/`，测试在 `backend/tests/<module>/`。

- `backend/src/main.cpp`：短生命周期 bootstrap；解析包、执行包状态准备，把精确包根路径快照传给 worker 后 `exec`。
- `backend/src/main_worker.cpp`：读取快照，加载 v1 插件 runtime，再启动 HTTP server。
- `backend/src/plugins/package.cpp`：校验 V1 manifest、平台模块和 frontend schema。
- `backend/src/plugins/packages.cpp`：ZIP 解包、安装状态、启停/卸载和核心 migration。
- `backend/src/plugins/runtime.cpp`：`dlopen`、factory/destroy、registrar 校验、冻结路由和渠道类型 registry。
- `backend/include/plugins/sdk.hpp`：公开 V1 SDK facade；`plugins/gateway.hpp`：公开 Gateway facade。
- `server/`：HTTP 解析、核心控制面路由和 v1 数据面适配入口。
- `channels/`：渠道与渠道组 store/API；渠道类型由 v1 插件 registry 提供模型和上游准备逻辑。
- `proxy/`、`request/`、`store/`：上游调度、流式传输、请求计价、ODB 和 schema。

## 运行模型

API 网关启动时：

1. bootstrap 连接 MySQL 并 `ensure_schema`，扫描系统包和用户包，解析依赖并记录下一次 worker 使用的包根路径。
2. worker 读取不可变快照，对每个当前平台模块执行 `dlopen(module, RTLD_NOW | RTLD_LOCAL)`。
3. worker 查找 `revlm_plugin_create_v1()` / `revlm_plugin_destroy_v1()`，调用注册器并校验路由、渠道类型、schema 和 migrations。
4. 注册校验成功后执行插件 migrations；成功者进入冻结 registry，失败者标记 `failed`，不注册其路由。
5. worker 暴露核心 `/api/*` 控制面和冻结插件提供的 `/v1/*` 数据面。V1 没有热加载或热卸载。

前端静态资源不由网关进程提供；渠道表单通过 `/api/plugins/channel-types` 获取声明式 schema。

## 数据合同

- 插件包必须使用 `format_version: 1` 和 `sdk_abi: "revlm-plugin-cpp-v1"`。
- 插件只能注册 `/v1/*` 的 GET/POST 数据面路由、渠道类型和 migrations。
- `frontend/channel-types.json` 是声明式配置；核心只执行 schema，不执行插件 JavaScript。
- 模型目录不是数据库表；由已加载的 v1 渠道类型 descriptor 提供。
- 数据库表 `plugin_installations` 的 `core_abi` 列保留为管理面兼容字段，存储 v1 `sdk_abi` 值。
- `plugin_migrations` 记录已执行 migration；卸载不回滚，也不删除插件业务数据。

## 相关页

- [API 手册](./api.md)
- [数据模型](./data-model.md)
- [插件包格式](../plugin-package-format.md)
- [C++ HTTP 边界](./router-boundaries.md)
