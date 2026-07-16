# Channel Groups 重写需求与缺口分析

> 状态：设计记录（**当前仅冻结 hpp API**；`cpp` 实现与调用方改造后续再做）。
> 基于 `backend/include/channels/channel_groups.hpp` 当前草案与全项目引用梳理。
> 日期：2026-07-01

## 1. 设计目标（已确认）

### 1.1 路由模型

- **外界不再有智能路由系统**（Scheduler 的 priority / promotion / ban / cooldown / affinity / credential 轮转等不再作为 channel group 选路依据）。Scheduler 模块清单见 **§10**。
- **失败切换**：上游报错时，调用 `ChannelGroup::next_channel()` 换下一个 channel 重试。
- **`next_channel` 轮询语义**（已确认）：
  - 组内维护私有 `pointer`（`int`），**即 `channels` 的下标**。
  - 每次调用：`pointer = (pointer + 1) % channels.size()`，当前 channel 为 `channels[pointer]`。
  - **前置条件**：`channels` 非空；空组不会被选用，`next_channel()` 不会被调用。
  - `pointer` 仅组内使用，**外界（admin / API / DB 层）不暴露、不管理**。

### 1.2 数据模型

| 决策 | 说明 |
|------|------|
| 成员即 `channels` | 不再定义 `ChannelGroupMemberDetail`；`list_channel_group_members` 等 Detail API 删除，直接读 `ChannelGroup::channels`。 |
| `price_multiplier` 用 `double` | 不再要 `normalize_price_multiplier()`。 |
| 删除 `created_at` / `updated_at` | `ChannelGroup` 与 admin JSON 均不再携带。 |
| 删除默认组 | 无 `get/set_default_channel_group_id`；admin `PUT .../default` 删除。 |
| 删除 pointer 对外 API | 无 `ChannelGroupPointer` 类型；无 `resolve/get/upsert_channel_group_pointer`；admin `.../pointer` 路由删除。 |
| 删除 `list_all_channel_group_members` | scheduler snapshot 不再按 member 表聚合。 |
| 删除 `list_used_upstream_channel_ids` | 不为 channel admin 列表单独查「是否被组引用」。 |

### 1.3 Channel 行为

- Channel **不再有超时保护**（与旧 scheduler / runtime 里的 probe、ban 等解耦）。

---

## 2. 新类型（目标对外 API）

来源：`backend/include/channels/channel_groups.hpp`（当前头文件草案）。

### 2.1 `ChannelGroup`

| 成员 | 类型 | 行号 | 意义 |
|------|------|------|------|
| `id` | `long long` | L16 | 组 ID |
| `name` | `std::string` | L17 | 组名；token 绑定、计费倍率查找仍靠 name |
| `description` | `std::string` | L18 | 描述（admin / 用户 token API 展示） |
| `price_multiplier` | `double` | L19 | 价格倍率 |
| `status` | `int` | L20 | `1` 启用，其它禁用 |
| `channels` | `std::vector<Channel>` | L21 | 组内 channel 列表（顺序即轮询顺序） |
| `next_channel()` | 方法 | L23 | 失败切换时轮询下一个 channel |
| `pointer` | `int`（private） | L26 | 轮询游标，仅 `next_channel` 使用 |

### 2.2 `ChannelGroupStore`

| 方法 | 行号 | 意义 |
|------|------|------|
| `ChannelGroupStore(odb::database &)` | L31 | 构造 |
| `list_channel_groups()` | L32 | 列出所有组（含 channels） |
| `get_channel_group_by_id(long long id)` | L33 | 按 ID 取组（含 channels） |
| `create_channel_group(name, description, price_multiplier)` | L35 | 建组；返回 `int` id，`0` 表示失败 |
| `update_channel_group(id, name, description, price_multiplier)` | L36 | 更新组元数据 |
| `delete_channel_group(long long id)` | L37 | 删除组 |
| `add_channel_group_member(long long id, long long channel_id)` | L38 | 向组追加单个 channel |
| `remove_channel_group_member(long long id, long long channel_id)` | L39 | 从组移除 channel |
| `create_channel_group_member(long long id, std::vector<Channel> channels)` | L40 | **覆写**组成员与顺序：由调用方构建好有序 `channels` 传入；替代旧 `reorder_channel_group_members` / admin `POST .../children/reorder` |

### 2.3 Admin 路由

| 符号 | 行号 | 意义 |
|------|------|------|
| `ChannelGroupsAdminParsedRequest` | L45–49 | HTTP 解析：`method` / `path` / `target` |
| `channel_groups_admin_route(...)` | L51–55 | `/api/admin/channel-groups.*` 入口 |

---

## 3. 明确删除的旧符号

以下旧 API / 类型在重写后**不再保留**（调用方需改或删）：

### 3.1 类型

- `ChannelGroupMemberDetail`
- `ChannelGroupPointer`

### 3.2 自由函数

- `normalize_price_multiplier(std::string_view)`

### 3.3 `ChannelGroupStore` 旧方法

- `list_channel_group_members` → 用 `get_channel_group_by_id(...).channels`
- `list_all_channel_group_members`
- `resolve_channel_group_pointer` / `get_channel_group_pointer` / `upsert_channel_group_pointer`
- `list_channel_group_pointer_snapshots`
- `get_default_channel_group_id` / `set_default_channel_group_id`
- `list_used_upstream_channel_ids`
- `add_channel_group_member_channel(..., priority, promotion)` → 简化为 `add_channel_group_member`
- `force_delete_channel_group` → 合并为 `delete_channel_group`

### 3.4 `ChannelGroup` 旧字段

- `created_at` / `updated_at`
- `price_multiplier` 的 `std::string` 表示

### 3.5 Admin HTTP（待收缩）

当前 `channel_groups_admin_api.cpp` 仍注册、但按新设计应删除的路由：

| 路由 | 原因 |
|------|------|
| `GET/PUT .../pointer` | pointer 内化 |
| `PUT .../default` | 无默认组 |
| `POST .../children/reorder` | 由 `create_channel_group_member` 覆写；admin 侧改为接收完整有序 `channels` 后调用 Store |

---

## 4. 重写后仍需要的功能（缺口）

> **范围说明**：本节除 hpp 冻结外，实现与调用方改造均**暂不推进**（按当前决策先只定头文件）。

在采纳 §1 前提下，后续实现阶段仍缺下列能力（头文件已有声明 ≠ 已实现）。

### 4.1 `ChannelGroupStore` 实现（后续）

| 能力 | 说明 |
|------|------|
| 全部 Store 方法实现 | `list/get/create/update/delete/add/remove/create_channel_group_member` 读写 DB |
| 加载时填充 `channels` | `list/get` join 成员表 → `vector<Channel>` |
| `next_channel` 实现 | 进程内 `pointer` 下标轮询（§1.1） |

### 4.2 Admin API 改造（P0）

`channel_groups_admin_api.cpp` 仍依赖已删除概念，需整体对齐新模型：

| 现状 | 目标 |
|------|------|
| JSON 含 `created_at`/`updated_at`/`is_default`/pointer 字段 | 删掉 |
| `channel_group_member_json(ChannelGroupMemberDetail)` | 改为序列化 `Channel` 或仅返回 channel id 列表 |
| `create` 接受 `status`、字符串倍率 | 对齐 `double` + 是否保留 `status`（§6） |
| `detail` 响应里的 `members` + 全量 `channels` 候选列表 | `members` → `group.channels`；候选列表逻辑可保留 |
| `notify_routing_change` | scheduler 删除后需新失效策略或删除（§6） |

### 4.3 数据面代理改造（P0）

当前 `gateway.cpp` / `responses_proxy.cpp` 通过 **Scheduler** 选路上游：

```
TokenAuth.groups → SchedulerConstraints → scheduler.select() → SchedulerSelection → upstream
失败 → scheduler.report() → ban/cooldown/换 credential
```

新模型目标链路（草案）：

```
TokenAuth.groups → 解析组名 → ChannelGroup（含 channels）→ 当前 channel 发请求
失败 → group.next_channel() → 重试（无 scheduler 状态机）
```

**缺口**：整条 proxy 路径尚未设计/实现；Scheduler、`ProxyRoutingDataSource`、`SchedulerRoutingSnapshot` 中与 group member/pointer 相关的部分待删除或替换。

### 4.4 Token 绑定（P0，模块在 `TokenStore`）

与 channel group **名称/ID 绑定**仍需要，实现在 `tokens.cpp` / `tokens.hpp`，不属 `ChannelGroupStore`，但依赖 `ChannelGroup` 类型：

| 方法 | 意义 |
|------|------|
| `list_channel_groups` | 用户选组列表 |
| `get_channel_group_by_id` / `get_channel_group_by_name` | 校验绑定目标 |
| `list/replace/list_effective_*_token_channel_group*` | token ↔ 组 CRUD；鉴权后填充 `TokenAuth::groups` |

**缺口**：

- `tokens.cpp` 仍读 `created_at`/`updated_at`、字符串 `price_multiplier` → 需对齐 `double` 与新字段。
- `get/set_default_channel_group_id` 在 `tokens.hpp` L86–87 → 按 §1 删除。
- `replace_token_channel_groups` 是否仍禁止绑定 `status!=1` 的组 → 行为保持即可。

### 4.5 用户 Token API（P1）

`http_dispatch.cpp` `token_channel_groups_response`（L1297+）序列化 `ChannelGroup`：`description`、`status`、`price_multiplier`。删除时间戳后字段对齐即可。

### 4.6 测试与垃圾清理（P1）

| 文件 | 处理方向 |
|------|----------|
| `channel_groups_test.cpp` | 删 `normalize_price_multiplier` 测试或整个文件 |
| `admin_channel_groups_contract_test.cpp` | 删 pointer/Detail 断言，改测 `channels` |
| 各 mysql 集成测试 | `add_channel_group_member_channel(id, ch, priority, promo)` → `add_channel_group_member`；`create` 签名对齐 |
| `app_settings_mysql_test.cpp` | 删默认组相关用例 |
| `scheduler_test.cpp` | scheduler 若整模块删除则一并删 |

### 4.7 Channel Admin「in_group」展示（P2）

`channel_admin_api.cpp` L701–766 用 `list_used_upstream_channel_ids` 给每个 channel 打 `in_group` 标记。

按 §1 删除该 API 后：**要么** admin JSON 去掉该字段，**要么** 用别的方式算（例如扫 `list_channel_groups` 合并 channels，但用户认为多余——倾向直接去掉 UI 字段）。

---

## 5. 与旧系统的映射（迁移时注意）

| 旧概念 | 新模型 |
|--------|--------|
| `channel_group_members` 行 + priority/promotion | `ChannelGroup::channels` 向量顺序 |
| DB `channel_group_pointers` 表 | 内存 `pointer`，`next_channel()` |
| Scheduler `select` + `report` | 单次选当前 channel + 失败 `next_channel` |
| `SchedulerRoutingDataSource::list_all_channel_group_members` | `ChannelGroupStore::list_channel_groups`（每组自带 channels） |
| `TokenStore::get_default_channel_group_id` | 删除；无默认组 fallback |

---

## 6. 已确认 / 仍待定

### 已确认

| 项 | 结论 |
|----|------|
| `next_channel` | `pointer = (pointer + 1) % channels.size()`；`pointer` 为 `channels` 下标 |
| 空 `channels` | 不选用该组；不调用 `next_channel` |
| 调顺序 | 外界构建有序 `vector<Channel>`，经 `create_channel_group_member(id, channels)` 覆写；无独立 reorder API |
| `create_channel_group_member` 签名 | `(long long id, std::vector<Channel> channels)`，无 `channel_id` |
| 当前范围 | **只冻结 `channel_groups.hpp`**；`.cpp` 与调用方后续再做 |

### 仍待定

1. **Scheduler 去留范围**：见 §10 清单；channel group 选路确定废弃，整模块是否删除、credential/endpoint 如何选路仍待定。
2. **组 `status`**：`ChannelGroup::status` 有字段，`create/update` Store 方法尚无 `status` 参数。
3. **`get_channel_group_by_id` 不存在时**：返回 `id==0` 的空组 / `optional` / 异常。
4. **`create_channel_group` 返回值**：维持 `int`（0=失败）或改 `optional<long long>`。
5. **`notify_routing_change`**、**`TokenStore` 重复 list**、**channel admin `in_group`**：实现阶段再定。

---

## 7. 当前头文件 vs 旧调用方签名差异（实现时会对照改）

| 旧调用 | 新头文件 | 备注 |
|--------|----------|------|
| `create_channel_group(name, desc, status)` 或带字符串倍率 | `create_channel_group(name, desc, double)` | 无 `status` 参数 |
| `update_channel_group(..., status, string_multiplier)` | `update_channel_group(..., double)` | 无 `status` |
| `add_channel_group_member_channel(g, ch, priority, promo)` | `add_channel_group_member(g, ch)` | |
| `reorder_channel_group_members(g, ids)` | `create_channel_group_member(g, channels)` | 调用方构建 `channels` |
| `get_channel_group_by_id` → `optional` | `ChannelGroup` 值类型 | 错误语义待定 |
| `force_delete_channel_group` | `delete_channel_group` | |

---

## 8. 建议实施顺序（仅规划，未执行）

1. ~~确认 §6，冻结 hpp API~~（进行中：`channel_groups.hpp`）。
2. 实现 `channel_groups.cpp`（Store + `next_channel`）。
3. 改 `channel_groups_admin_api.cpp` 对齐新 JSON / 路由。
4. 改 `tokens.cpp` 对齐 `ChannelGroup` 新字段；删默认组。
5. 替换 proxy 选路（去 Scheduler 或大幅瘦身）。
6. 删 scheduler / routing_data_source 中 member/pointer 路径。
7. 更新集成测试。

---

## 9. 相关文件索引

| 文件 | 关系 |
|------|------|
| `backend/include/channels/channel_groups.hpp` | 新 API 声明 |
| `backend/src/channels/channel_groups.cpp` | 待实现 |
| `backend/src/channels/channel_groups_admin_api.cpp` | Admin，需大改 |
| `backend/include/users/tokens.hpp` | Token ↔ 组绑定 |
| `backend/src/proxy_request/gateway.cpp` | 现用 Scheduler |
| `backend/src/proxy_request/responses_proxy.cpp` | 现用 Scheduler |
| `backend/include/scheduler/scheduler.hpp` | 待删除或瘦身 |
| `backend/include/proxy_request/routing_data_source.hpp` | 待删除或替换 |
| `backend/src/channels/channel_admin_api.cpp` | `list_used_upstream_channel_ids` 一处 |

---

## 10. Scheduler 模块清单

来源：`backend/include/scheduler/scheduler.hpp`（实现 `backend/src/scheduler/scheduler.cpp`；测试 `backend/tests/scheduler/scheduler_test.cpp`）。

与 channel group 重写的关系：**§1 仅废弃其中「按组选 channel」的智能逻辑**；下列其余能力是否一并删除尚未拍板。

### 10.1 枚举 / 结果类型

| 符号 | 意义 |
|------|------|
| `SchedulerApi` | `openai` / `anthropic`，约束 API 类型 |
| `SchedulerCredentialType` | `openai_compatible` / `anthropic` |
| `SchedulerFailureScope` | 失败归因：`credential` / `endpoint` / `channel` / `model` |
| `SchedulerSelection` | 一次选路结果：channel、endpoint、`base_url`、credential、model binding、`route_group_multiplier` 等 |
| `SchedulerResult` | 上游结果反馈：`success` / `retriable` / `status_code` / `failure_scope` / `cooldown_until` |
| `SchedulerConstraints` | 选路约束：允许组名、组顺序、`required_channel_id`、排除集、顺序 failover、`requested_model` 等 |
| `SchedulerBanEntry` | channel 封禁条目：`until` + `streak` |

### 10.2 路由快照 `SchedulerRoutingSnapshot`

内存中的全量路由图（`rebuild_routing_snapshot()` 从 `SchedulerRoutingDataSource` 加载）：

| 字段 | 意义 |
|------|------|
| `channels` | 全部 `UpstreamChannel` |
| `endpoints_by_channel` | channel → 多个 `UpstreamEndpoint` |
| `openai_credentials_by_endpoint` / `anthropic_credentials_by_endpoint` | endpoint → 凭据列表 |
| `channel_groups_by_id` / `channel_groups_by_name` | 组元数据 |
| `group_members_by_group_id` | **旧** `ChannelGroupMemberDetail` 列表（重写后应删除） |
| `group_pointers_by_group_id` | **旧** `ChannelGroupPointer`（重写后应删除） |
| `group_names_by_channel` | channel 所属组名 |
| `model_bindings_by_public_id` | 模型 → `ChannelModelBinding` |
| `bindings_for_model` / `channel_supports_model` | 模型与 channel 是否匹配 |

### 10.3 数据源 `SchedulerRoutingDataSource`（虚接口）

| 方法 | 意义 |
|------|------|
| `list_upstream_channels` | 渠道列表 |
| `list_all_upstream_endpoints` | 全部 endpoint |
| `list_all_openai_compatible_credentials` / `list_all_anthropic_credentials` | 全部凭据 |
| `list_channel_groups` | 组列表 |
| `list_all_channel_group_members` | **旧**，重写后删除 |
| `list_channel_group_pointer_snapshots` | **旧**，重写后删除 |

实现类：`ProxyRoutingDataSource`（`backend/include/proxy_request/routing_data_source.hpp`），组装 `ChannelStore` + `CredentialStore` + `ChannelGroupStore`。

### 10.4 运行时状态 `SchedulerState`

跨请求/可挂 Redis 的**智能状态机**：

| 能力 | 意义 |
|------|------|
| **Affinity** | `set/get_affinity`：用户粘滞到某 channel |
| **RPM** | `record_rpm` / `rpm`：凭据每分钟请求计数 |
| **Cooldown** | credential / endpoint / model 冷却：`set_*_cooldown`、`is_*_cooling`、`clear_*` |
| **Channel 失败分** | `record_channel_failure` / `channel_fail_score` / `reset_channel_fail_score` |
| **Channel ban** | `ban_channel` / `ban_channel_immediate` / `is_channel_banned` / `clear_channel_ban` / `sweep_expired_channel_bans` |
| **Probe** | `is_channel_probe_due` / `try_claim_channel_probe` / `release_channel_probe_claim` 等（超时探测） |
| **Redis** | `attach_redis_state_store` / `load_redis_state` |

### 10.5 核心 `Scheduler` 类

| 方法 | 意义 |
|------|------|
| `rebuild_routing_snapshot` | 重建快照 |
| `routing_snapshot` / `routing_generation` | 读快照与版本号 |
| `route_key_hash` | 会话级路由键哈希（rendezvous） |
| **`select(user_id, route_key_hash, constraints)`** | **主选路**：按组顺序、priority、promotion、ban、cooldown、affinity、凭据轮转选 `SchedulerSelection` |
| **`report(selection, result)`** | **反馈**：失败时触发 ban、cooldown、失败分等 |
| `set_affinity_ttl` / `set_rpm_window` / `set_cooldown_base` / `set_probe_claim_ttl` | 调参 |

私有逻辑（头文件可见）：`select_from_ordered_groups`（按 token 绑定组顺序选成员）、`select_channel_candidate`、`select_credential`、顺序 failover 缓存等。

### 10.6 工具函数

| 函数 | 意义 |
|------|------|
| `scheduler_credential_type_name` | 凭据类型名 |
| `parse_scheduler_credential_key` | 解析 `credential_key` |
| `scheduler_rendezvous_score` | rendezvous 哈希分 |

### 10.7 当前调用方（channel group 废弃后需替换）

| 文件 | 用法 |
|------|------|
| `backend/src/proxy_request/gateway.cpp` | `ProxyGatewayContext` 持有 `Scheduler`；chat/completions 选路 |
| `backend/src/proxy_request/responses_proxy.cpp` | `scheduler.select` + `report` 循环重试 |
| `backend/include/proxy_request/upstream.hpp` | `UpstreamExecutor` 入参为 `SchedulerSelection` |
| `backend/src/proxy_request/upstream.cpp` | 按 selection 拼请求、执行上游 |

**与新版 `ChannelGroup::next_channel` 的重叠部分**：仅「组内换 channel」；Scheduler 还承担 endpoint/credential 选择、ban/cooldown/affinity，这些在新模型里**尚未定义替代方案**。
