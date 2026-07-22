# /v1 Proxy 请求统一重构

> 范围：仅 /v1/* 代理链路（chat/completions、messages、responses），不涉及 /api 管理端通信

## 动机

/v1 代理链路当前每个请求同时穿过 **4 层异构结构体**：

```
httplib::Request
    → ParsedRequest + build_raw_http_request → raw_request 文本
        → RequestContext（包裹：parsed, raw_request, request_id, usage_event_id, client_ip, set_cookie）
            → json envelope（从 httplib::Request 投影 8 个 flat key，proxy handler 入口格式）
                → Request (ODB, 30+ 字段，既当运行时载体又当 DB 行)
```

| 冗余 | 例子 |
|---|---|
| 同数据多存 | `request_id` 在 RequestContext, envelope, Request 各存一份 |
| 俄国套娃 | `build_raw_http_request` 拼成 HTTP 文本 → proxy 入口再 `json::parse(body)` 拆开 |
| 裸指针 | `Request::pricing_model` 指向 Channel 栈上 Model，用完须手动 `= nullptr` |

## 设计决策：为什么用类型化 struct 而不是 JSON

审查确认了统一载体的核心思路，但否决了 JSON 方案。原因：

1. **`revlm::json::operator[]`（非常量）在拼写错误的键上静默插入 null** — 写入 `request["upstream"]["pricing"]["input_prcie"]`（拼写错误）会编译、运行，并在零警告的情况下提交零美元计费。
2. **`as_double()` 拒绝字符串类型的数字** — 如果代码路径错误写入了 `"2.50"`（字符串）而不是 `2.5`（double），`as_double()` 静默返回 empty，`value_or(0)` 给出零。
3. **即使读取时出现拼写错误，非常量 `operator[]` 也会插入垃圾键** — `(void)request["auth"]["user_ld"]` 将 `"user_ld": null` 插入树中。
4. **类型化 struct 使得这 3 类错误成为编译错误。** 这就是务实的安全保障。

## 目标数据结构

```cpp
// backend/include/request/proxy_request.hpp （新文件）

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace revlm {

// ── http: 入口时从 httplib::Request 一次填完 ──
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;            // 原始上游请求体
    std::string client_ip;
    std::vector<std::pair<std::string, std::string>> headers; // 不含 authorization/x-api-key
    // NOTE: headers 使用 vector<pair<string,string>> 类型，与 UpstreamHeader 结构一致。
    // 未来考虑统一为 UpstreamHeader 类型以消除两套表示。
};

// ── auth: 鉴权后填入 ──
struct Auth {
    long long user_id = 0;
    long long token_id = 0;
    long long channel_group_id = 0;
};

// ── pricing: 值语义替代 Request::pricing_model 裸指针 ──
struct Pricing {
    double input_price = 0.0;
    double output_price = 0.0;
    double cache_read_price = 0.0;
    double cache_creation_1h_price = 0.0;
    double cache_creation_5m_price = 0.0;
};

// ── usage: Gateway / SSE 写入的 token 计数（赋值语义 =，last-wins）──
struct Usage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_creation_1h_tokens = 0;
    int cache_creation_5m_tokens = 0;
};

// ── upstream: proxy handler 选 channel 后填入 ──
struct Upstream {
    long long channel_id = 0;
    std::string model_name;
    std::string service_tier;
    int status_code = 0;          // 唯一权威来源：Gateway 写入；finish_proxy_usage 和 commit_proxy_usage 都读这个字段
    int latency_ms = 0;           // 仅落库（chat/messages 非流式当前未设，已知问题不在此重构解决）
    int first_token_latency_ms = 0;
    std::string response_id;      // 上游响应后填入（非流式在 handler 内部，流式在 Gateway SSE 处理中）
    double channel_multiplier = 1.0;
    // tier_multiplier 默认 1.0（合法计费值，不再用 0.0 哨兵）：
    // - chat/messages: 保持默认 1.0（或不显式赋值）
    // - responses: apply_responses_billing 在 parse_billing 后写入最终值
    // - 流式路径：stream pump 完成后基于最终 input_tokens 重新计算
    double tier_multiplier = 1.0;

    Pricing pricing;              // fill_pricing_from_model 一次性填充
};

// ── ProxyRequest: 一份数据，从入口到落库全程一块内存 ──
struct ProxyRequest {
    long long id = 0;
    std::string request_id;
    std::string time;            // MySQL datetime，入口时设为请求到达时间（见下文 BREAKING CHANGE 说明）
    bool is_stream = false;      // 来自客户端请求的 “stream” 字段，是请求模态属性，非上游特征

    HttpRequest http;
    Auth auth;
    Upstream upstream;
    Usage usage;                 // 上游返回的 token 计数，独立子结构

    // 未来预留（当前未使用，与 ODB Request 的可空字段对应）：
    // std::string error_class;
    // std::string error_message;
};

// 计费纯函数：定义于此头文件，不放进 gateway
void fill_pricing_from_model(Pricing &pricing, const Model &model);
double compute_usd(const ProxyRequest &pr);

} // namespace revlm
```

### 字段变更总结（相对打平稿）

| 变更 | 旧位置 | 新位置 | 原因 |
|---|---|---|---|
| `is_stream` | Upstream | ProxyRequest 顶层 | 来自客户端请求，是请求模态属性，非上游特征 |
| `Pricing` struct | 曾打平进 Upstream | **独立 struct**，嵌套为 `Upstream::pricing` | pricing 是一组费率，与 routing 字段分组不同 |
| `Usage` struct | 曾打平进 Upstream | **独立 struct**，挂在 `ProxyRequest::usage` | usage 是上游返回数据，与 upstream routing 分组不同 |
| `tier_multiplier` 默认值 | 0.0 哨兵 | **1.0** | 1.0 是合法默认计费乘数；不再用 0.0 区分「未设」 |
| `tier_multiplier` 流式重算 | 从未重算 | on_complete 回调或 commit 内惰性重算 | CRITICAL: 4.0x 乘数在流式响应中永远无效 |
| `Pricing` 构造 | 默认构造 + `copy_from()` | `fill_pricing_from_model(pricing, model)` 自由函数一次性填充 | 消除全零中间态 |
| `compute_usd` 归属 | 曾写进 gateway 清单 | **仅** `proxy_request.hpp` | 计费纯函数跟载体走，gateway 只调用 |
| `status_code` 权威来源 | `res.status` + `upstream.status_code` 双重 | 统一为 `upstream.status_code` | 消除双重权威来源 |
| `headers` 类型 | `vector<pair<string,string>>` | 保持（加注释说明未来考虑统一为 UpstreamHeader） | 减少变更范围 |

### 字段填充时序

```
make_request(req)
  ├── id (= make_usage_event_id()), request_id, time (= to_mysql_datetime(now))
  ├── http.method, http.path, http.body, http.client_ip, http.headers (strip authorization/x-api-key)
  │
[v1_http lambda: authenticate_api_token]
  ├── auth.user_id, auth.token_id, auth.channel_group_id           ← 鉴权不过则直接 401 返回
  │
[proxy handler: run_chat_completions / run_messages / handle_responses]
  ├── is_stream = parse_json_bool_field(req.body, "stream")        ← 客户端请求属性
  ├── upstream.channel_id, upstream.model_name, upstream.service_tier          ← 选 channel 后立即填入
  ├── fill_pricing_from_model(upstream.pricing, *model)              ← 一次性填入费率，无无效中间态
  ├── upstream.channel_multiplier                                     ← 从 channel.price_multiplier
  ├── upstream.tier_multiplier:
  │     chat/messages: 保持默认 1.0
  │     responses: apply_responses_billing 写入最终值（覆盖默认 1.0）
  ├── upstream.response_id                                            ← 上游响应后填入（非流式：handler 内部；流式：Gateway SSE 处理中）
  │
[仅 responses: apply_responses_billing 内部]
  ├── parse_billing_request_from_body → 填 usage.input_tokens 等
  ├── upstream.tier_multiplier = tier_multiplier_for(model, requested_tier, response_tier, usage.input_tokens)
  │   ↑ 必须在 parse_billing 之后、compute_usd 之前计算
  │
[Gateway::finalize / SSE usage 事件]
  ├── usage.input_tokens, usage.output_tokens,
  │   usage.cache_read_tokens, usage.cache_creation_*_tokens         ← 从 usage chunk 写入（赋值语义，last-wins）
  │
[仅 responses 流式: on_complete 或 commit_proxy_usage 内部]
  ├── upstream.tier_multiplier = tier_multiplier_for(model, requested_tier, response_tier, usage.input_tokens)
  │   ↑ 流式泵取完成后，usage 的最终 token 计数已写入，此时重新计算 tier_multiplier
  │   ↑ 确保 priority + high_context (input>272K) → 4.0x 可达
  │
[on_complete / finish_proxy_usage]
  └── commit_proxy_usage(pr)
        ├── 运行时 guard: tier_multiplier <= 0.0 || channel_multiplier <= 0.0 → ERROR + return 0.0
        ├── 从 pr 各子树字段构造 ODB Request
        ├── compute_usd(pr) = (pricing.input_price * usage.input_tokens / 1e6 + ...)
        │                     * upstream.tier_multiplier * upstream.channel_multiplier
        ├── ODB Request 设 date = pr.time.substr(0, 10)
        ├── UserStore::debit_user_balance_usd(auth.user_id, usd)
        ├── Request::commit(finished_at) → persist → apply_total()
        └── （不再读取 pricing_model — 已删除）
```

### 线程安全保证

流式路径中，`ProxyRequest` 被 move 进 `shared_ptr` 管理的 Shared 上下文。Gateway 持有 `ProxyRequest &`（非 const）在 SSE 事件到达时写入 `usage.input_tokens` 等 int 字段。`on_usage` 回调以 `const ProxyRequest &` 读取这些字段。

**设计保证**：Gateway 的写入和 on_usage/on_complete 回调在**同一事件循环线程**中顺序执行。`usage` 的 token 字段使用**赋值语义**（last-wins）而非累加（+=），与当前代码行为一致——若上游发送多个 usage 事件（如 Anthropic message_start + message_delta + message_stop），最终的完整 usage 覆盖中间的 partial usage。此假设对 Responses API 正确（`response.completed` 携带完整 usage），但对增量式上游可能需要调整。实现中在 `commit_proxy_usage` 加注释说明此语义。

### 字段决策

| 字段 | 决策 |
|---|---|
| `http.headers` | 入口时 strip `authorization` / `x-api-key`，**使用 `lowercase_ascii` 做大小写不敏感比较**。HTTP 规范允许 header 名任意大小写（`Authorization`/`authorization`/`AUTHORIZATION` 均合法），精确字符串比较会导致 API key 泄露至上游。存储为 `vector<pair<string,string>>`（与 UpstreamHeader 结构一致，未来考虑统一为 UpstreamHeader 类型） |
| `http.path` | 替代旧 envelope 的 `method` + `path` 两个 key — 下游路由用 path 识别端点 |
| `http.body` | 保留原始字符串，proxy 入口解析 JSON 提取 model 名。**channel/model 选择完成后立即 `clear()` + `shrink_to_fit()` 释放内存** — 50MB 上限下 100 并发流式请求如不释放将占用 5GB |
| `upstream.pricing` | **替代 `Request::pricing_model` 裸指针** — 值语义，无生命周期依赖。通过 `fill_pricing_from_model(upstream.pricing, model)` 一次性填充，确保无全零中间态 |
| `upstream.tier_multiplier` | 默认 **1.0**。chat/messages 保持默认；responses 由 `apply_responses_billing` 覆盖为最终值。**流式路径 pump 完成后重新计算**（基于最终 `usage.input_tokens`）。忘记覆盖时按 1.0 计费（可能少收 2x/4x），不再用 0.0 哨兵 |
| `upstream.channel_multiplier` | proxy handler 在选 channel 时填入 |
| `upstream.status_code` | **唯一权威来源**。Gateway 写入，`finish_proxy_usage` 和 `commit_proxy_usage` 都读此字段（不再读 res.status） |
| `upstream.response_id` | 上游响应后填入：非流式在 handler 内部、流式在 Gateway SSE 处理中 |
| `upstream.latency_ms` | 仅用于落库。当前 chat/messages 非流式路径未设此值（现存问题，不在此重构解决），commit_proxy_usage 中此字段有注释说明已知 0-value 来源 |
| `usage.*` | Gateway::finalize 写入（赋值语义 =，last-wins），commit 时一次性读 |
| `is_stream` | ProxyRequest 顶层字段，来自客户端请求体的 `stream` 解析 |
| `time` | 在入口就填 MySQL datetime。**这是 BREAKING CHANGE**：旧语义为 commit 时刻（`request_timestamp_now()`），新语义为请求到达时间。见下文兼容性章节 |
| `error_class` / `error_message` | ProxyRequest 暂不加对应字段。当前代理路径中从未设置这些 ODB 可空字段，重构后保持未设置（与旧行为一致）。在 `commit_proxy_usage` 注释中标注有意留空，未来需要时再加 |

### 纵深防御

**header 剥离**：`make_request` 负责剥离 `authorization`/`x-api-key` 头。作为纵深防御，`build_proxy_upstream_request` 内部对每个 header name 做 `lowercase_ascii` 比较，跳过敏感 key 并记录 WARNING 日志（意味着上游代码有 bug）。

**compute_usd 运行时守卫**（定义在 `proxy_request.hpp`）：防止 Release 构建下负数乘数导致反向扣款（用户余额增加）：

```cpp
if (pr.upstream.tier_multiplier <= 0.0 || pr.upstream.channel_multiplier <= 0.0) {
    ERROR("compute_usd: invalid multiplier tier=%.4f channel=%.4f",
          pr.upstream.tier_multiplier, pr.upstream.channel_multiplier);
    return 0.0;
}
```

注：默认 `tier_multiplier = 1.0`，此 guard 只拦 `<= 0`（bug / 负值），不再承担「未设置」检测。

**ID 生成**：`make_usage_event_id()` 当前使用 `steady_clock` XOR 16-bit 原子计数器。为消除重复 ID 风险（同一纳秒两请求 → 静默数据丢失），改用 `std::random_device` 或 `std::mt19937_64` 生成密码学强度 ID。若保持当前算法，需在 DB `requests.id` 列加 UNIQUE 约束使重复变为显式错误。

## ODB Request 角色变更

ODB `Request` 类变为**仅用于 DB 投影**：

- `BOOST_DESCRIBE_STRUCT(Request, ...)` **保留** — `/api` 读取路径仍然需要它（`request_to_user_event_json`、`request_to_admin_event_json`）
- `Request::pricing_model` **删除** — usd 由 commit_proxy_usage 在构造 ODB 对象之前预计算（调用 `compute_usd`）
- `Request::commit()` — 删除第 50-53 行（`pricing_model->name` fallback + `solve_price()` fallback）。`usd` 已由 `commit_proxy_usage` 设置。保留 `time.empty()` 时 `request_timestamp_now()` fallback（防御性编程，简化为 1 个默认值），同时在 commit() 开头加注释说明对 /v1 路径 time 由 make_request 保证非空
- `Request::solve_price()` — 保留，简化为直接 `return usd`
- `RequestStore::query()` 中 `hydrate_request_model` 调用 — 同步删除

## 兼容性

### BREAKING CHANGE: time 字段语义

`time` 字段从 commit 时刻改为请求到达时间。影响范围：

- `RequestStore::query()` 的 start/end_exclusive 过滤
- `request_to_user_event_json()` 的 ISO8601 转换
- `aggregate_window()` 的 min/max 计算
- `date` 派生字段（`time.substr(0,10)`）

**迁移方案（两阶段）**：

1. ≥第一阶段：保留旧 `time` 语义（`commit_proxy_usage` 中调用 `request_timestamp_now()` 设置 time），同时新增 `arrived_at` 列存储入口到达时间
2. 后续阶段：管理面板确认适配后，将 `time` 切换为入口到达时间语义

### /api 管理端影响

`compute_pricing_breakdown()` 不再返回单价（`*_usd_per_1m`）或分项成本；只返回 token 计数、倍率与落库的 `final_cost_usd`。管理/用户 usage 明细 UI 同步去掉「×$/1M」公式。

### input_tokens 端点的 DB 行行为

当前代码对 `/v1/responses/input_tokens` 端点**不产生 DB 行**（`handle_responses_proxy_request` 在此路径直接 `return {}`，不设置 `channel_id` 和 `model_name`，`finish_proxy_usage` 因此提前返回）。

重构后**保持此行为**：新 `handle_responses_proxy_request` 在 `input_tokens` 路径不设置 `pr.upstream.channel_id` 和 `pr.upstream.model_name`。`commit_proxy_usage` 开头的 `channel_id <= 0` 和 `model_name.empty()` 早退检查（见下文）确保不产生 DB 行。

## 迁移

采用**分阶段部署**以降低风险（替代原设计的单一原子变更）：

### 阶段 1（独立 commit，零破坏）

- 新增 `backend/include/request/proxy_request.hpp`（`ProxyRequest` / `Pricing` / `Usage` / `Upstream` + `fill_pricing_from_model` + `compute_usd`）
- `compute_usd` / `fill_pricing_from_model` 作为自由函数引入（不依赖任何旧代码）
- **此 commit 不破坏编译，可独立部署**

### 阶段 2（原子 commit，编译期耦合）

- `gateway.cpp/hpp`：核心签名变更（调用 `compute_usd`，不定义它）
- `request.cpp/hpp`：pricing_model 删除、solve_price 简化
- `openai_chat.cpp/hpp`、`anthropics_messages.cpp/hpp`、`openai_responses.cpp/hpp`：handler 迁移
- `http_dispatch.cpp/hpp`：接线层重写
- 测试文件适配

**回滚方案**：恢复 8 个文件 + 删除 1 个新文件。文件清单见下文。

### 文件改动清单

| # | 文件 | 变更 |
|---|---|---|
| 1 | **新文件** `backend/include/request/proxy_request.hpp` | `ProxyRequest` + `HttpRequest` / `Auth` / `Upstream` / `Pricing` / `Usage` + `fill_pricing_from_model` + `compute_usd` |
| 2 | `http_dispatch.cpp` | `make_request`；`v1_http` 重写；删除 `build_proxy_envelope`；`log_access` 签名改为 `(request_id, method, path, status)`；`/v1` handler 传 `ProxyRequest &`。`build_raw_http_request`、`inject_request_metadata`、`ParsedRequest`、`RequestContext` **保留**供 `/api` 路径使用 |
| 3 | `http_dispatch.hpp` | 移除 `inject_request_metadata`；新增 `make_request` |
| 4 | `openai_chat.cpp/hpp` | 签名 `(ProxyRequest &)`；`fill_usage_from_success` 删除；pricing 通过 `fill_pricing_from_model(upstream.pricing, …)` 一次性填入；不再需要 `pricing_model = nullptr` |
| 5 | `anthropics_messages.cpp/hpp` | 同上 |
| 6 | `openai_responses.cpp/hpp` | 签名 `(ProxyRequest &)`；`tier_multiplier` 由默认 1.0 覆盖为最终值；流式 pump 完成后重新计算 tier_multiplier；`ResponsesProxyExecuteOptions::on_usage` 改为接受 `const ProxyRequest &` |
| 7 | `gateway.cpp/hpp` | `Gateway(ProxyRequest &)`；删除 `Model*`/乘数参数；`make_gateway(kind, pr)`；`build_proxy_upstream_request(pr, path)` 内部加防御性 header strip；`commit_proxy_usage(pr)` 调用 `compute_usd(pr)`（定义在 proxy_request.hpp）；`parse_billing_request_from_body` 参数改为 `ProxyRequest &`；`apply_upstream_gateway_stream` 移动 `ProxyRequest` |
| 8 | `request.cpp/hpp` | `pricing_model` 删除；`solve_price()` 简化为 `return usd`；`commit()` 删除第 50-53 行，保留 time.empty() 的防御性 fallback；`hydrate_request_model` 删除；`RequestStore::query()` 中同步删除 hydrate 调用 |
| 9 | `json_convert.hpp` | **不变**（Request 的 BOOST_DESCRIBE_STRUCT 保留） |
| 10 | 测试文件 | `http_server_*`/`gateway_*` 测试适配新签名 + 新增计费核心测试 |

### 关键实现检查点

- `compute_usd` 定义在 `proxy_request.hpp`；被调用时 `pr.upstream.tier_multiplier` **应为**最终值（responses 流式须先重算）。运行时 guard 拦 `<= 0`
- **responses 流式路径**：`Gateway::finalize` 写入最终 token 计数后，在 `on_complete` 回调或 `commit_proxy_usage` 内部调用 `tier_multiplier_for` 重新计算 `pr.upstream.tier_multiplier`。此重算必须在 `compute_usd` 之前
- `make_request` 剥离 header 时必须使用 `lowercase_ascii` 做大小写不敏感比较
- `build_proxy_upstream_request` 内部添加防御性 strip 并记录 WARNING
- channel/model 选择完成后调用 `pr.http.body.clear()` + `shrink_to_fit()` 释放 body 内存
- `commit_proxy_usage` 开头早退检查：`id <= 0`、`user_id <= 0`、`token_id <= 0`、`channel_id <= 0`、`model_name.empty()` — 覆盖所有调用路径（包括流式路径绕过 `finish_proxy_usage` 的情况）
- 鉴权失败时 `commit_proxy_usage` 被跳过需记录 WARNING 日志

## 消除的代码

| 删除 | 行数（估计） | 原因 |
|---|---|---|
| `build_proxy_envelope` | ~25 | 被 ProxyRequest struct 替代 |
| `fill_usage_from_success`（openai_chat + anthropics_messages） | ~20×2 | 内联为直接字段赋值 |
| `Request::pricing_model` + `hydrate_request_model` | ~5 + ~5 | 用值语义替换裸指针 |
| `Request::commit()` 第 50-53 行 | ~4 | price/model_name fallback 不再需要 |
| `Gateway` 构造函数参数 | ~5 | 移除 Model*、乘数 |
| `make_gateway` 参数 | ~4 | 同上 |
| `assign_request_correlation` | ~5 | 内联（仅剩 2 个调用点） |
| **总计删除** | **~80-100 行** | |

**保留**（/api 路径仍需要，仅 /v1 不再调用）：`build_raw_http_request`、`inject_request_metadata`、`ParsedRequest`、`RequestContext`、`make_request_context`

新增代码：`ProxyRequest` 及相关子 struct（~70 行）、`fill_pricing_from_model`（~6 行）、`make_request`（~30 行）、`compute_usd`（~12 行含运行时 guard，在 proxy_request.hpp）、`commit_proxy_usage` 重写（~50 行含早退检查）、v1_http lambda 内联（~20 行新增，但删除了旧的 v1_http 封装）。净代码量大致持平或略有减少。

## 测试覆盖

### 必须新增的测试（计费核心路径）

| # | 测试 | 说明 |
|---|---|---|
| 1 | responses Path B tier_multiplier 回归 | 4 个子场景：① default + low input → 1.0 ② priority only → 2.0 ③ high_context only → 2.0 ④ priority + high_context (>272K) → 4.0。直接 SQL 查询 requests 表验证 usd 和 tier_multiplier |
| 2 | 流式 tier_multiplier 重算 | 模拟 SSE 流 pump 前 input_tokens=0 → tier=1.0，pump 后 input_tokens=300K → tier 重算为 4.0。验证 compute_usd 使用的是重算后的值 |
| 3 | header 大小写不敏感剥离 | 构造 `AUTHORIZATION`、`authorization`、`Authorization`、`X-API-KEY`、`x-api-key` 变体，验证 make_request 和 build_proxy_upstream_request 均正确剥离 |
| 4 | compute_usd 公式正确性 | 纯函数测试（不依赖 DB）：已知 pricing/usage/multiplier → assert 手算结果。覆盖 5 种 token 类型和 multiplier=4.0 极端场景 |
| 5 | commit_proxy_usage ODB 字段完整性 | 构造完整 ProxyRequest（所有字段非默认值），SQL 查询逐字段验证。用 BOOST_DESCRIBE_STRUCT 成员数做编译期守卫 |
| 6 | 非流式 usage 解析 | chat/messages 非流式路径：构造已知 body（含 usage JSON），调用 parse_billing_request_from_body → 验证 `pr.usage.input_tokens` / `output_tokens` 等 token 计数 |
| 7 | 流式 usage last-wins 语义 | 模拟 SSE 流返回多个 usage chunk（partial + final），验证最终 token 计数为 final 而非累加值 |
| 8 | commit_proxy_usage 早退守卫 | 验证 channel_id=0、model_name="" 时 commit_proxy_usage 返回 false 且不扣费 |
| 9 | compute_usd 负数乘数运行时 guard | 构造 tier_multiplier=-1.0 的 ProxyRequest → 验证返回 0.0 且不扣费（不依赖 assert） |

### 需适配的现有测试

| 测试文件 | 变更 |
|---|---|
| `http_server_mysql_chat_completions_test.cpp` | 改为按值传递 `ProxyRequest` 或 mock 封装器 |
| `http_server_mysql_messages_test.cpp` | 同上 |
| `http_server_mysql_responses_test.cpp` | 需要 `ProxyRequest` 而非 `Request` + envelope；新增 tier_multiplier 子测试 |
| `gateway_failover_mysql_test.cpp` | `Gateway` 构造不再需要 `Model*`、乘数 |
| `upstream_test.cpp` | `build_proxy_upstream_request` 接受 `ProxyRequest` |

## 实施顺序（分阶段）

### 阶段 1：独立 commit（零破坏）

| 步骤 | 文件 | 说明 |
|---|---|---|
| 1 | `backend/include/request/proxy_request.hpp` | **新文件**。`ProxyRequest` + `Pricing` / `Usage` / `Upstream` + `fill_pricing_from_model` + `compute_usd`。无依赖，不破坏编译 |

### 阶段 2：原子 commit（按依赖链顺序实施）

| 步骤 | 文件 | 说明 |
|---|---|---|
| 2 | `backend/include/proxy/gateway.hpp` + `gateway.cpp` | 核心签名变更：`Gateway`、`make_gateway`、`build_proxy_upstream_request`、`commit_proxy_usage`（调用而非定义 `compute_usd`）、`parse_billing_request_from_body`、`apply_upstream_gateway_stream`、`proxy_stream_commit_usage`、`finish_proxy_usage` |
| 3 | `backend/include/request/request.hpp` + `request.cpp` | `pricing_model` 删除、`solve_price()` 简化、`commit()` 清理、`hydrate_request_model` 删除 |
| 4 | `openai_chat.cpp/hpp` | 最简单的 handler — 先迁移以验证模式 |
| 5 | `anthropics_messages.cpp/hpp` | 与 openai_chat 并行 — 相同模式 |
| 6 | `openai_responses.cpp/hpp` | **最复杂** — 三个流式路径、`tier_multiplier` 覆盖默认 1.0 + post-pump 重算、`on_usage` 回调适配。风险最高；最后迁移 |
| 7 | `http_dispatch.cpp/hpp` | 接线层：`v1_http` 重写、`log_access` 签名变更、`make_request` |
| 8 | 测试文件 | 适配新签名 + 新增计费核心测试 |

**回滚方案**：恢复 8 个文件 + 删除 proxy_request.hpp。文件清单：`proxy_request.hpp`、`http_dispatch.cpp/hpp`、`openai_chat.cpp/hpp`、`anthropics_messages.cpp/hpp`、`openai_responses.cpp/hpp`、`gateway.cpp/hpp`、`request.cpp/hpp`、测试文件。

## 不变

- **对外 API**：不改
- **DB 表结构**：不改（后续可能新增持久化定价列，但不在此重构范围）
- **/api 管理端基础功能**：读取路径不改（`session.cpp`、`user_api.cpp`、`channel_api.cpp`、`channel_groups_api.cpp`、`security.cpp` 均不参与）。usage detail 的 `pricing_breakdown` 不再含单价/分项成本字段
- **`log_access` 签名变更**：从 `(RequestContext&, int)` 到 `(request_id, method, path, status)`。`make_http_handler` 内部从 ctx 提取字段，调用新签名。`v1_http` lambda 直接从 ProxyRequest 读取。对 /api 路径无功能影响
- **`BOOST_DESCRIBE_STRUCT(Request)`**：保留。`request_to_user_event_json` 和 `request_to_admin_event_json` 继续为 /api 读取路径工作

## 已知退化

1. **`error_class` 和 `error_message` 未被 ProxyRequest 携带。** ODB `Request` 有可空的 `error_class`/`error_message` 字段，当前代理路径中从未设置它们。重构后仍保持未设置 — 这是现有行为，未变更。在 `commit_proxy_usage` 注释中标注有意留空，未来需要时可在 ProxyRequest 添加对应字段。

2. **`latency_ms` 在 chat/messages 非流式路径中永远为 0。** 当前 `fill_usage_from_success` 从不设置 latency_ms，重构后此 gap 仍在。注释中标注已知问题，不在本次重构解决。
