# 数据模型

本页定义 C++ 后端要实现的当前数据合同。它来自 ODB 实体注解
（`#pragma db`）、空库 ODB 基线，以及 `backend/migrations/` 版本化 SQL 经启动时
`ensure_schema` 应用到 MySQL 后的 live schema，
再对照仍然属于产品运行态的读写路径整理而成。历史迁移中出现过、
但本页没有列入的对象，都不能被当作当前模型。

## 合同边界

- 事实来源是 ODB 实体、版本化 SQL 迁移与 `ensure_schema` 后的 live schema；本文是人工维护的合同摘要，不是完整 DDL dump。
- C++ 数据层只实现本文列出的表和字段语义。
- 历史 Go 文件名只能作为迁移证据，不能让已经删除的表、列、配置项回到 C++ 合同里。

## 已删除对象

这些对象已经不属于当前产品状态；C++ 后端不要实现、查询或兼容它们：

- 订阅/套餐域：`subscription_plans`、`user_subscriptions`、`subscription_orders`、`requests.subscription_id`。
- DB 模型配置域：`managed_models`、`channel_models`。模型目录由插件在注册阶段登记，不建表。
- 旧会话/ACL 域：`user_sessions`、`oauth_apps` 这一套旧 OAuth app 表、`main_groups`、`main_group_subgroups`。
- 插件状态域：`plugin_installations`、`plugin_migrations`。
- 旧 pending/运维/对象引用域：`usage_pending_events`、`usage_pending_hourly_stats`、`usage_subscription_pending_events`、`usage_subscription_pending_hourly_stats`、`admin_k8s_operations`、`openai_object_refs`、`error_passthrough_rules`、`audit_events`。
- 旧用量事件结算列：`requests.status`（曾用 `committed` 等终态标记；已由 `0005_drop_request_status.sql` 删除）。
- 旧关系列：`users.channel_group`、`upstream_channels.groups`、`token_channel_groups.channel_group_name`。
- 已删除上游表：`upstream_channels`、`upstream_endpoints`、`channel_group_pointers`（由 `0131_channels_refactor.sql` 迁移至 `channels` 并内联 `base_url`）。
- 旧 token 列：`user_tokens.token_hint`、`user_tokens.created_at`、`user_tokens.revoked_at`、`user_tokens.last_used_at`。
- 旧用户列：`users.created_at`。
- 旧余额表：`user_balances`（余额已并入 `users.balance_usd`）。
- 旧渠道请求改写列：`upstream_channels.allow_service_tier`、`fast_mode`、`disable_store`、`allow_safety_identifier`、`setting`、`param_override`、`header_override`、`status_code_mapping`、`model_suffix_preserve`、`request_body_blacklist`、`request_body_whitelist`。

## 用户、Token 与会话

### `users`

登录账号表。

字段：

- `id`: 用户主键。
- `email`: 登录邮箱，唯一。
- `username`: 登录账号名，唯一，大小写敏感，只允许英数字。
- `password_hash`: 密码哈希。
- `role`: 角色，核心值为 `root`、`user`。
- `status`: 用户状态。
- `balance_usd`: PayGO 美元余额（`double`），默认 `0`。

语义要点：

- 用户不再直接持有渠道组；可访问边界来自 `user_tokens` 到 `token_channel_groups`。
- `created_at` / `updated_at` 不存在，Web session 不依赖用户行时间戳做失效判断。
- 管理员通过 `POST /api/admin/users/:id/balance` 手动入账；数据面请求从 `balance_usd` 扣费。

### `user_tokens`

用户 API token 表。

字段：

- `id`: token 主键。
- `user_id`: 所属用户。
- `name`: token 名称，可空。
- `token_hash`: token 哈希，唯一。
- `token_plain`: 明文 token，可空；只用于控制台 reveal/rotate 之后的展示。
- `status`: 状态，`1=启用`、`0=禁用`。

语义要点：

- revoke/delete 以 `status`、删除行和级联清理绑定为准，不保留 `revoked_at`。
- token 可访问渠道组在 `token_channel_groups`。

### `sessions`

浏览器 Web 会话表。Cookie `revlm_session` 存不透明 token；库中只存其 SHA-256 哈希。

字段：

- `token_hash`: cookie 原文的 SHA-256 hex，主键。
- `user_id`: 所属用户。
- `expires_at`: 过期时间。

## 计费

PayGO 余额存在 `users.balance_usd`，不再使用独立的 `user_balances` 表。

## 上游资源与路由

### `channels`

上游接入单元，对应 C++ `Channel`。

字段：

- `id`: channel 主键。
- `type`: 上游类型字符串；核心不限制枚举，插件自行解释。
- `name`: 渠道名。
- `status`: 状态，`1=启用`、`0=禁用`。
- `priority`: 调度优先级，越大越优先。
- `base_url`: 上游基地址。
- `api_key`: 上游 API key（明文）。
- `config_json`: 插件专属的任意 JSON 对象，默认 `{}`。

语义要点：

- `base_url` 与 `api_key` 直接存在 channel 行上；每个 channel 最多一个 upstream key。
- 渠道组成员关系在 `channel_group_members`；不存在 channel 上的 `groups` 列。

## 渠道组、模型与绑定

### 模型目录

模型目录不是数据库表。插件在注册阶段按 `ChannelGroup.type` 登记自己的目录，核心只按该键索引和转发。
共享模型结构只有 `id`、`name` 和插件自定义的 `pricing` JSON；核心不解析 `pricing`，也没有固定的
input/output/cache 价格列、`owned_by` 或 `icon_url`。

语义要点：

- 系统 `OpenAI`、`Anthropic` 包分别为 `openai_compatible`、`anthropic` 两个 `ChannelGroup.type` 登记目录。
- 新协议不需要改数据库或核心代码；对应插件决定模型与可达性。
- token 可用模型由 token 绑定渠道组的 type 决定，因此 `/v1/models` 不会把不同协议的模型混在一起。
- 不存在独立模型配置页、`managed_models` 表或 `channel_models` 表。

## 插件包状态

插件状态不在数据库里。启用/停用与待卸载意图记录在插件目录下的一个 `state.json`，其余（已安装了什么、
加载成功还是失败、失败原因）都是进程内运行态，冷启动时重新确定。`plugin_installations` 与
`plugin_migrations` 两张表已随 v3 删除（`0011_plugins.sql`、`0012_plugin_core_abi.sql` 一并移除）：
插件迁移的幂等性由插件自己的迁移入口负责，核心不再替它记账。

### `channel_groups`

渠道组定义表。

字段：

- `id`: 渠道组主键。
- `name`: 组名，唯一。
- `description`: 描述。
- `price_multiplier`: 该组价格倍率（DB `decimal(25,6)`；C++/JSON API 为 `double` / number）。
- `status`: 状态，`1=启用`、`0=禁用`。

### `channel_group_members`

渠道组成员关系表，只支持组内直接挂 channel。

字段：

- `id`: 关系主键；自增顺序决定组内 channel 轮询顺序（`ORDER BY channel_group_id, id`）。
- `channel_group_id`: 父渠道组。
- `channel_id`: 成员 channel ID。

约束：

- `UNIQUE (channel_group_id, channel_id)`：同组不重复 channel。
- `KEY (channel_group_id, id)`：加速按组加载成员顺序。

语义要点：

- 一条记录只表示「组包含 channel」。
- 不存在组套组；`member_group_id`、`priority`、`promotion` 已删除。

### `token_channel_groups`

token 级渠道组绑定表。

字段：

- `token_id`: token ID。
- `channel_group_id`: 绑定的渠道组 ID。
- `priority`: token 级优先级。

主键：

- `(token_id, channel_group_id)`

语义要点：

- 展示名从 `channel_groups.name` 派生。
- 生效时只保留仍存在且 `channel_groups.status=1` 的渠道组。

## 用量与聚合

### `requests`

数据面请求的原始事实表。一次客户端请求只产生一行，无论核心为它轮转过多少个候选 Channel。

字段：

- `id`: 请求记录主键（显式写入，非自增）。
- `time`: 请求发生时间。
- `request_id`: 客户端可见的请求标识（`X-Request-Id`，缺省时由网关生成），可空。
- `response_id`: 上游返回的关联标识，可空。
- `endpoint`: 请求路径，可空。
- `method`: HTTP 方法，可空。
- `status_code`: 上游或网关状态码。
- `latency_ms`: 总延迟。
- `first_token_latency_ms`: 首字延迟。
- `error_message`: 错误摘要，可空。
- `user_id`: 用户 ID。
- `token_id`: token ID。
- `channel_id`: 实际尝试过的上游 channel。
- `channel_group_multiplier`: 提交时取得的渠道组倍率快照（`double`）。
- `usage_details`: 插件写入的原始 usage JSON（TEXT，默认 `{}`）；核心不解析。
- `model`: 模型名，可空。
- `usd`: 最终金额快照（`double`），等于插件给出的基础金额乘以 `channel_group_multiplier`。

语义要点：

- 写入去重边界是显式 `id`（同一 `id` 只落一行）。
- 只有真正到达过某个 Channel 的请求才落库：没有可用 Channel、没有匹配路由的请求不写记录也不计费。
- 协议专属的一切（token 数、缓存、service tier、是否流式）都在 `usage_details` 里，不再有独立列。
- 无事件结算状态列；余额走 `users.balance_usd`，与用量窗口解耦。

### `request_totals`

按 `(user_id, token_id, date)` 汇总的滚动统计，随请求提交同步更新。

字段：

- `user_id`、`token_id`、`date`: 复合主键。
- `requests`: 请求数。
- `usd`: 金额合计（`double`）。
- `first_token_latency_sum`: 首字延迟求和，用于算平均值。

语义要点：

- 只汇总核心维度：请求数、金额、首字延迟。token/缓存列不存在，也没有替代列。
- 它不替代 `requests`；缺口需要回退原始行重建。
