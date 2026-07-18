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

- 订阅/套餐域：`subscription_plans`、`user_subscriptions`、`subscription_orders`、`usage_events.subscription_id`。
- DB 模型配置域：`managed_models`、`channel_models`。模型目录由 C++ 常量实现，不建表。
- 旧会话/ACL 域：`user_sessions`、`oauth_apps` 这一套旧 OAuth app 表、`main_groups`、`main_group_subgroups`。
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

### `session_bindings`

Web/Codex 路由会话绑定表。

字段：

- `user_id`: 用户 ID。
- `route_key_hash`: 路由键哈希。
- `payload_json`: 绑定载荷。
- `expires_at`: 过期时间。

主键：

- `(user_id, route_key_hash)`

## 计费

PayGO 余额存在 `users.balance_usd`，不再使用独立的 `user_balances` 表。

## 上游资源与路由

### `channels`

上游接入单元，对应 C++ `Channel`。

字段：

- `id`: channel 主键。
- `type`: 上游类型字符串，`openai_compatible` | `anthropic`。
- `name`: 渠道名。
- `status`: 状态，`1=启用`、`0=禁用`。
- `priority`: 调度优先级，越大越优先。
- `base_url`: 上游基地址。
- `api_key`: 上游 API key（明文）。

语义要点：

- `base_url` 与 `api_key` 直接存在 channel 行上；每个 channel 最多一个 upstream key。
- 渠道组成员关系在 `channel_group_members`；不存在 channel 上的 `groups` 列。

## 渠道组、模型与绑定

### 内置模型目录

模型目录不是数据库表。C++ 后端用具名静态 `Model` 常量提供模型列表、归属方、价格和缓存价格；`Channel` 构造时按 `type` 填充成员 `models`。

语义要点：

- `openai_compatible` 类型的启用 channel 默认提供内置 OpenAI 模型。
- `anthropic` 类型的启用 channel 默认提供内置 Anthropic 模型。
- token 可用模型由 token 的有效渠道组里能访问到的启用 channel 类型决定。
- 不存在独立模型配置页、`managed_models` 表或 `channel_models` 表。

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

### `usage_events`

数据面请求的原始事实表。运行时 `Request` / `UsageEvent` 与此表对齐；HTTP `request_id` 即显式 `id`（bigint）。

字段：

- `id`: 用量事件主键（显式写入，非自增）；与网关 `request_id` 相同。
- `time`: 请求发生时间。
- `endpoint`: API endpoint，可空。
- `method`: HTTP 方法，可空。
- `status_code`: 上游或网关状态码。
- `latency_ms`: 总延迟。
- `first_token_latency_ms`: 首字延迟。
- `error_class`: 错误分类，可空。
- `error_message`: 错误摘要，可空。
- `user_id`: 用户 ID。
- `token_id`: token ID。
- `channel_id`: 命中的上游 channel（默认 0）。
- `model`: 计费/转发侧模型名，可空。
- `service_tier`: 实际生效服务层级，可空。
- `input_tokens`: 输入 token，可空。
- `cache_read_tokens`: 缓存读取 token，可空。
- `cache_creation_5m_tokens`: 5m 缓存创建 token，可空。
- `cache_creation_1h_tokens`: 1h 缓存创建 token，可空。
- `output_tokens`: 输出 token，可空。
- `tier_multiplier`: 服务层级倍率（DB/C++ `double`；usage detail JSON 为 number）。
- `channel_multiplier`: 渠道组倍率（DB/C++ `double`；usage detail JSON 为 number）。
- `is_stream`: 是否流式请求。
- `usd`: 落库时的费用快照（`double`）；`commit` 时由当时 `pricing_model` 算出后写入。

语义要点：

- 写入去重边界是显式 `id`（同一 `id` 只落一行）。
- 金额落在 `requests.usd`；读历史用量时用该快照（`solve_price()` 在无 `pricing_model` 时回退到 `usd`）。
- 无事件结算状态列（`status` / pending hold 已删除）；余额走 `users.balance_usd`，与用量窗口解耦。
- 按天/小时/分钟的统计表是 `usage_events` 的聚合物，不是原始事实表。

### 聚合表

这些表按粒度缓存查询结果：

- `usage_daily_stats`、`usage_hourly_stats`、`usage_minute_stats`: 用户/token/channel 维度。
- `usage_daily_scope_stats`、`usage_hourly_scope_stats`、`usage_minute_scope_stats`: scope 维度。
- `usage_daily_model_stats`、`usage_hourly_model_stats`、`usage_minute_model_stats`: 用户/model 维度。
- `usage_daily_stats_backfilled`、`usage_hourly_stats_backfilled`、`usage_minute_stats_backfilled`: 聚合覆盖标记。

语义要点：

- 聚合表不替代 `usage_events`；缺口和无效覆盖需要回退原始事件或重建。
- 聚合只汇总 token/请求/延迟样本；不存 `usd`/`cost_usd`（API 层再算价）。
- 缓存字段为 `cache_read_tokens` 与合并后的 `cache_creation_tokens`（5m+1h）。
- subscription scope 已被 `0122_drop_subscriptions.sql` 清掉，不再是有效 scope。
