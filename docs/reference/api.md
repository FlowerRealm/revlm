# API 手册

本页记录当前 C++ 后端已经实现的 HTTP 面。路由事实来源是 `backend/src/server/http_server.cpp` 及其分发的 admin/API 模块。

## 通用返回约定

- `/api/*` JSON 接口统一返回 `{"success":<bool>,"message":"...","data":...}` 结构。
- 认证失败、参数错误、业务失败通常仍返回 HTTP 200，但 `success=false`。
- `/readyz` 直接使用普通 HTTP 状态码和纯文本内容。

## 系统接口

- `GET /readyz`（draining 时返回 503）

## 用户会话

- `POST /api/user/register`
- `POST /api/user/login`
- `GET /api/user/self`
- `GET /api/user/logout`
- `GET /api/user/models/detail`
- `GET /api/dashboard`
- `POST /api/account/email`
- `POST /api/account/password`

语义要点：

- 登录成功后通过 `revlm_session` Cookie（不透明 session id）维持浏览器会话。
- register / login / self / logout 由 MySQL 中的 `users` 与 `sessions` 驱动。

## Token 管理

- `GET /api/token`
- `POST /api/token`
- `GET /api/token/:id/reveal`
- `POST /api/token/:id/rotate`
- `POST /api/token/:id/revoke`
- `DELETE /api/token/:id`
- `GET /api/token/:id/channel-groups`
- `PUT /api/token/:id/channel-groups`

## 用量查询

- `GET /api/usage/windows`
- `GET /api/usage/events`
- `GET /api/usage/events/:id/detail`
- `GET /api/usage/timeseries`

## 渠道管理

- `GET /api/channel/page`
- `POST /api/channel`
- `PUT /api/channel`
- `DELETE /api/channel/:id`
- `GET /api/channel/:id/timeseries`

`POST /api/channel` 与 `PUT /api/channel` 接受 `type`、可选 `key`（upstream API key，明文存储）和
`config_json`（任意插件自定义对象）。核心不维护渠道类型 registry；插件按自己的普通 C++ 替换
实现解释这些字段。

渠道组与 usage pricing breakdown 中的倍率字段（`price_multiplier`、`tier_multiplier`、`channel_multiplier`）均为 JSON number。

## 渠道组管理

- `GET /api/admin/channel-groups`
- `POST /api/admin/channel-groups`
- `GET /api/admin/channel-groups/:id/detail`
- `PUT /api/admin/channel-groups/:id`
- `DELETE /api/admin/channel-groups/:id`
- `PUT /api/admin/channel-groups/:id/default`
- `GET /api/admin/channel-groups/:id/pointer`
- `PUT /api/admin/channel-groups/:id/pointer`
- `POST /api/admin/channel-groups/:id/children/channels`
- `DELETE /api/admin/channel-groups/:id/children/channels/:channelId`
- `POST /api/admin/channel-groups/:id/children/reorder`

## 插件管理（root）

- `GET /api/admin/plugins`
- `POST /api/admin/plugins/upload`（原始 `.revlm-plugin` ZIP，`X-Plugin-Filename` 必填）
- `POST /api/admin/plugins/:plugin_id/enable`
- `POST /api/admin/plugins/:plugin_id/disable`
- `DELETE /api/admin/plugins/:plugin_id`

这些操作仅记录待重启状态。上传和运行插件等同于信任本机代码；卸载不执行 down migration，也不删除插件数据。

前端插件发现接口：

- `GET /api/plugins/frontend`
- `GET /api/plugins/frontend/:plugin_id/:asset_path`

它们不是前端 SDK；第二个接口只从当前 worker 启动时的包快照提供 `frontend/` 下的任意 ESM、chunk、
CSS 或资源文件，上传/停用后不会在运行中的 worker 内变化。

## 用户管理

- `GET /api/admin/users`
- `POST /api/admin/users`
- `PUT /api/admin/users/:id`
- `POST /api/admin/users/:id/password`
- `POST /api/admin/users/:id/balance`
- `DELETE /api/admin/users/:id`

## 管理用量

- `GET /api/admin/dashboard`
- `GET /api/admin/usage`
- `GET /api/admin/usage/timeseries`
- `GET /api/admin/usage/users/suggest`
- `GET /api/admin/usage/channels/suggest`
- `GET /api/admin/usage/models/suggest`
- `GET /api/admin/usage/events/:id/detail`

## 余额

- `GET /api/billing/balance`

用户余额只读查询。充值由管理员通过 `POST /api/admin/users/:id/balance` 手动入账，写入 `users.balance_usd`。

## 数据面

系统 `OpenAI`、`Anthropic` 插件预加载后提供以下协议实现：

- `GET /v1/models`
- `GET /v1/models/:id`
- `POST /v1/chat/completions`
- `POST /v1/messages`
- `POST /v1/responses`
- `POST /v1/responses/input_tokens`

数据面请求走 token 认证与上游调度；用量经 `Request::commit()` 落库。`/v1/models` 按 token 绑定的
渠道组确定唯一插件类型并返回该插件的目录。插件也可覆盖整个 `revlm_register_http_routes` 普通函数并
定义不同的 HTTP 面；本页只描述当前系统插件的兼容合同。

## 尚未实现

- `/oauth/*` 通用 OAuth 入口（除 `GET /auth/callback` 外）

其余合同面见 [data-model.md](./data-model.md)。
