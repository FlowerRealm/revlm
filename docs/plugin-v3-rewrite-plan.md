# 插件 v3 重写工单

本页是把当前代码对齐到 [CONTEXT.md](../CONTEXT.md) 与 ADR 0001–0008 的执行计划。它描述要做什么和按什么顺序做，不描述如何实现。

结论先行：当前分支约 500 行插件宿主代码实现的是文档已经删除的概念（依赖图、`targets`、`core_abi`、版本目录、`active` 符号链接、安装状态表、SQL 迁移表），而 v3 需要的注册表、控制面 C ABI 和协议处理器一个都不存在。这不是重构，是重写宿主与数据面两层。

## 目标规模

| 目标 | 现状 | 重写后 |
|---|---|---|
| 插件宿主 | 1327 | ~505 |
| `proxy/gateway.cpp` | 1421 | ~310 |
| `main.cpp` + `main_worker.cpp` | 174 | ~63 |
| 插件侧链式样板（submodule） | ~58 | 0 |

参考：realagent 的 `extension/loader` 用 626 行支持四种插件类型、依赖注入与运行时启停，因此 505 是保守估计。

## 阶段

阶段之间是严格依赖关系，不要并行。每个阶段结束时构建与测试必须是绿的。

顺序为 P0 → P1a → **P2/P3/P4（一个 flag day）** → P1b → P5 → P6。

当前进度：P0、P1a、P2、P3、P4 已完成。全树构建无错误无警告，27 个测试全绿，其中数据面测试走真实的 `dlopen` + 注册路径（见下）。P5 前端与 P6 文档已随之更新。P1b 只剩 `Channel` 的 `type` / `price_multiplier` / `config_json` 三列——`Model` 与 `requests` 表两项已随本次落地。

flag day 过程中出现了若干计划外的修正，都已记录到相应文档而不是留在代码里：

- 失败切换循环归属从插件改回核心（[ADR 0009](adr/0009-core-owned-failover-loop.md)），推翻了 ADR 0003 的对应部分。
- 流式响应从「处理器同步写客户端」改为交接式：HTTP 服务器只在请求处理函数返回后才交出可写 sink，同步写在这个宿主上根本做不到。见 ADR 0009「流是交接，不是同步写」。
- 轮转循环补上下界：环形轮转必须有终点，组内每个 Channel 各试一次后核心返回 HTTP 502。原文「不设重试上限」会让全部上游不可用时请求永远挂住——真实测试第一次跑通数据面就撞上了这个。
- 路由表增加以 `/` 结尾的前缀键，`GET /v1/models/:id` 才有地方落；核心仍然不解析路径。
- 客户端 sink 增加存活探测：上游沉默期间只靠写入发现不了已经离开的客户端。

一并修掉的、被旧机制掩盖的问题：

- `plugin::load_plugins()` 从来没有调用点，`revlm` 对每个 `/v1/*` 都回 HTTP 500。
- `revlm_prepare_upstream` 等四个预加载钩子已无人实现，默认实现会抛异常——即每次上游请求都失败。连同 `models/catalog.hpp` 一起删除。
- 13 个数据面/用量测试在 `REVLM_TEST_MYSQL_DSN` 未设时静默跳过，而 CI 之外没人设它：它们一直是绿的，却从没跑过。改用会自建容器的 `prepare_mysql_test_env`，随后暴露出上面几个真实缺陷。
- 请求记录主键来自进程内计数器且从 1 开始，重启后与既有行冲突；`Request::commit` 视之为「已写过」直接返回成功，于是重启后最初的若干次请求既不落库也不计费。改为启动时从 `MAX(id)` 起算。

### 为什么 P2、P3、P4 合成一次

初稿要求每个阶段结束时构建与测试都是绿的，而 P2 单独执行做不到。P2 删掉 bootstrap 与 `execv`，`LD_PRELOAD` 随之消失，两个插件的四个符号覆盖点和它们注册的 `/v1/*` 端点同时失效；替代通道——注册表与协议处理器——要 P3 与 P4 才存在。中间那个状态既编得过又跑不通，是最坏的一种绿。

考虑过让新旧宿主并存一段时间再切换，放弃了：那会让「插件是否启用」同时有两个真相，正是 ADR 0001 反对的重复状态。

因此 P2、P3、P4 作为一次原子改动执行，自底向上：契约头文件 → 宿主 → 核心分发 → 插件适配 → 删除旧宿主，末尾统一验证。`ProxyRequest` 的拍平也并入这一次而不留在 P1b——P3 本来就要重写它的每一个调用点，分两次做等于把同一批插件代码改两遍。

P1b 因此只剩 `Channel`、`Model` 与 `requests` 表三项（后两项已随 flag day 落地，实际剩 `Channel`）。

### P0 构建前置

不改动任何插件逻辑，只让后续阶段有地可落。

- `CMakeLists.txt`：主可执行文件启用 `ENABLE_EXPORTS`（`-rdynamic`），否则 `dlopen` 载入的插件解析不到核心符号。
- `cmake/Dependencies.cmake`：`find_package(libzip)`；`Dockerfile` 增加 `libzip-dev`，两个架构都要。
- `util/json.hpp`：引入 `strict_from<T>()`（缺键即失败、一次列全所有缺失键、返回 `std::expected<T, std::string>`），`to_json` 改为 `boost::json::value_from`。参考 `~/realagent/realagent/core/include/json.hpp`。
- `util/json_convert.hpp`：删除手写的 `mp_for_each` + `if constexpr` 序列化实现，Boost.JSON 的 Describe 集成已覆盖 nullable/optional/容器。

**验收**：全量构建通过，现有测试不变绿。

### 为什么 P1 拆成 P1a 与 P1b

工单初稿把数据模型对齐整块放在最前，那是错的。勘测表明被删的三样东西是插件当前赖以工作的机制，不是核心内部细节：

- `Channel.type` 是插件的自我识别键。`OpenAI/plugin.cpp:128,178,307,322` 与 `Anthropic/plugin.cpp:95,156,177` 用 `channel.type == k_channel_type` 门控每一个路由决策。
- `Gateway::fill_success_pricing()` 是插件把价格交回核心的唯一通道，它在基类里调用 `fill_pricing_from_model()` 并读 `channel.price_multiplier`。
- 两个插件直接写入 `ProxyRequest` 的五个嵌套结构（`OpenAI/plugin.cpp:112-121,161-171,188-269`、`Anthropic/plugin.cpp:68-88,113`）。

在 P3/P4 交付注册表路由与协议处理器之前删掉它们，插件既编不过也没有替代通道，「每阶段必须绿」无法成立。因此只把**增量**部分提前，**破坏性**部分推到插件不再引用它们之后。

### P1a 增量：`ChannelGroup.type`

只加不删，插件与现有测试不受影响。

- `ChannelGroup` 新增 `type`，自由字符串，核心不校验取值（ADR 0005、CONTEXT）。落到类体、store 层 SQL、`BOOST_DESCRIBE_STRUCT`、创建/更新 API 与前端编辑表单。
- 新建 `backend/migrations/0013_channel_group_type.sql`。现有迁移到 `0012_plugin_core_abi.sql` 为止。
- `http_dispatch.cpp:117-129` 的 `channel_group_type()` 目前靠「所有成员渠道的 `type` 必须一致」反推组类型。有了真列之后它退化为一次字段读取，但**本阶段不删**——删它属于 P1b。

**验收**：构建通过，现有测试不变绿。

### P1b 破坏性：删字段（在 P4 之后执行）

这是唯一的 flag day，会同时打断 proxy、channels、models、store、tests 和前端。单独成一个可回退的提交序列。

| 类型 | 动作 | 依据 |
|---|---|---|
| `Channel` | 删 `type`、`price_multiplier`、`config_json`、`models`，收敛为 `id`/`name`/`status`/`priority`/`base_url`/`api_key` | ADR 0005 |
| `Model` | 删 `owned_by`、`icon_url` 与五个价格字段（`input_price`、`output_price`、`cache_read_price`、`cache_creation_1h_price`、`cache_creation_5m_price`），收敛为 `id`/`name`/`pricing`(JSON) | CONTEXT「模型」 |
| `ProxyRequest` | 拍平：删 `HttpRequest`/`Auth`/`Pricing`/`Usage`/`Upstream` 五个嵌套结构，收敛为约 19 个平坦字段 | ADR 0004、CONTEXT「核心记录维度」 |
| `requests` 表 | 删 `tier_multiplier`、`service_tier`、`is_stream`、`error_class` 与五个 token 列；新增 `usage_details`(JSON)、`protocol_cost_usd`；`channel_multiplier` 改名 `channel_group_multiplier` | ADR 0004 |

`ProxyRequest` 的判定标准是「核心是否按它聚合或过滤」，不是「每个协议是否都有这个概念」。保留 `model_name`、`first_token_latency_ms`、`status_code`、`latency_ms`、上游关联标识；协议专属的一切进 `usage_details`。

同时删除 `fill_pricing_from_model()` 与 `compute_usd()`——核心不再从 token 算钱，插件直接提交 `protocol_cost_usd`。

`Model` 不落库：它由插件 ABI 在运行时产出（`models/catalog.hpp`），因此这一列改动没有迁移，只有结构与前端。

`requests` 的待删列同样**没有**对应的迁移文件——它们来自 ODB 内嵌 schema（见 `0001_baseline.sql` 注释），因此必须新写一个 `ALTER TABLE` 迁移，不存在可改的旧文件。

`RequestTotal`（`request/request.hpp:27-39`）持有自己一套 token 聚合列，由上述被删字段喂。它不在 CONTEXT 的核心记录维度里，删源字段后会静默变陈旧，必须在同一个提交里一起处理。

`channels.type` 直接改 schema，不写回填迁移（ADR 0005 已决）。

**验收**：构建通过；`backend/tests` 中涉及计费与渠道的用例改写后全绿。

### P2 插件宿主重写

删除后新建，不做增量改造。

**删除**
- `backend/src/plugins/packages.cpp`（762）、`package.cpp`（361）、`api.cpp`（80）及三个头文件
- `backend/migrations/0011_plugins.sql`、`0012_plugin_core_abi.sql`
- `backend/src/main.cpp` 的 bootstrap 逻辑与 `execv`；`main_worker.cpp` 合回 `main.cpp`
- 测试：`preload_override.cpp`、`preload_probe.cpp`、`preload_symbols_test.cpp`、`provider_preload_probe.cpp`、`provider_preload_symbols_test.cpp`

**新建**（预算 ~505）

| 模块 | 行 | 职责 |
|---|---|---|
| 清单解析 | 15 | `strict_from<PluginManifest>`，6 字段，`abi_version` 强校验 |
| 包扫描 | 55 | `packages/<id>/` 单层，`backend/<amd\|arm>/` 取唯一 `.so` |
| 安装 | 90 | libzip 解包、条目校验、staging、原子替换 |
| dlopen 宿主 | 90 | 加载、`migrate`、`register`、`cleanup`、错误捕获 |
| 路由表 | 70 | `(method, path, group_type) → handler`，插入冲突即报错 |
| 前端资产 | 35 | 入口清单与静态文件 |
| 管理端点 | 85 | 列表 / 上传 / 启停 / 卸载 |
| 插件状态 | 20 | 进程内 `known` 与 `loaded` 两张表，`to_json` 直接出站 |
| 头文件 | 45 | `PluginManifest`（入站）与 `PluginInfo`（出站）分开 |

`PluginManifest` 与 `PluginInfo` 必须是两个结构。把 `dir`/`status`/`error` 混进清单结构，会让严格解构把它们误报成缺键——这正是现有 `PluginInstallation` 十一个字段的病根。

禁用清单写入核心配置，不使用文件系统标记；文件系统只是已安装包的真相。

**验收**：装 / 启 / 停 / 卸载四条路径有集成测试；两个插件同键注册时启动报错并只让后注册者失败。

### P3 数据面重写

**删除**
- `class Gateway` 及九个虚函数、`usage_gateway_factory()`、`channel_ok()`
- `Gateway::run`/`run_stream`/`handle`×2（合计 361 行）
- `ProxyUpstreamResponse`/`UpstreamSession` 与两个 `stream_gateway_session_*`（198 行）
- `apply_upstream_gateway_stream`（89 行）
- `ScheduledUpstreamExecution` 与 `ScheduledUpstreamStreamExecution`（合并为一个 `std::expected`）

**移交插件**
- SSE 分帧、`contains_usage_object`、`find_first_model`、`handle_sse_event`（182 行）——CONTEXT 明确「核心不递归扫描 SSE」
- `build_proxy_upstream_request`、`remove_json_field`（67 行）
- `fill_success_pricing`、`should_bill_non_stream`、`make_upstream`、`no_available_channel_message`

**核心保留**（~350）：失败切换循环、上游传输、向客户端写出、请求提交，加上鉴权、路由、SSRF 校验、余额预检。合并 `UpstreamResponse` 与 `UpstreamStreamResponse` 为单一结构（`body` + `optional<rest>`，`rest` 有值即流式），插件面对的传输接口从两套变一套。

失败切换循环归核心（[ADR 0009](adr/0009-core-owned-failover-loop.md)）：核心取候选、调用协议处理器、按返回的 `Done` / `NextCandidate` 裁决决定继续或退出，退出后在唯一出口提交一次。核心不读上游状态码判断可恢复性。协议处理器因此处理的是**一次尝试**而不是整次请求，插件里不出现 `while`，也没有可调用的提交接口。比初稿的 ~310 多约四十行，换掉的是每个插件各写一份的轮转与提交逻辑。

`http_dispatch.cpp`：删除 `/v1/models` 与 `/v1/models/:model_id` 两条路由及 `data_plane_models_response`、`data_plane_model_retrieve_response`——模型列表端点归协议处理器。

**验收**：两个协议的非流式与流式各一条端到端用例；候选切换与提交只发生一次的用例。

### P4 插件侧（submodule `plugins/revlm-plugin`）

- 删 `plugins/common/preload_chain.hpp` 与两个插件中所有 `next_symbol` 调用
- 四个 `revlm_*` 覆盖点改为单一注册入口，登记路由与模型目录
- 删掉所有 `channel.type == k_channel_type` 门控。注册表已按 `(method, path, ChannelGroup.type)` 选中处理器，插件被调用时不需要再自证身份——这是这一阶段真正消除的特殊情况，`Channel.type` 的最后一批读者由此消失，P1b 才得以执行。
- `Gateway::fill_success_pricing()` 的覆盖改为在提交时直接给出 `protocol_cost_usd` 与 `usage_details`
- 协议处理器吸收从 P3 移交的 SSE 解析与上游构造
- `plugin.json` 改 6 字段；平台目录 `backend/linux-amd64/` 改为 `backend/amd/`
- `packaging/build-package.py` 与 CI 跟随
- 删除 `test_provider_catalog.cpp` 中依赖符号链的部分

### P5 前端

- `PluginsPage.tsx` 适配新的 `PluginInfo` 字段（`status`/`error`/`type`/`description`）
- `modelPricingDisplay.ts` 适配插件自定义 `pricing` JSON，不再假设固定价格字段
- `ChannelsPage`/`ChannelCommonTab` 移除 `type`、`price_multiplier`、`config_json`
- `ChannelGroupsPage` 增加 `type` 编辑

### P6 文档收尾

`docs/reference/` 下 `architecture.md`、`api.md`、`data-model.md`、`proxy-response-gateway.md`、`channel-groups-rewrite.md`，以及 `docs/deployment/overview.md` 和 `README.md`，仍在描述 `LD_PRELOAD`、预加载与 `Gateway`。等 P3 落地后一次性改写，避免写下尚未实现的内容。

## 风险

**P1b 是唯一的断点。** 四个结构同时改，proxy、channels、models、store、tests、前端一起红。单独成分支，测试全绿后再开 P5。

**P3 的行为等价性没有自动化保障。** 现有流式路径的用例覆盖有限，SSE 解析从核心搬到插件时容易丢边界行为（首 token 延迟统计、客户端断开、空闲超时）。建议 P3 之前先补流式端到端用例，用它们作为搬迁前后的对照。

**`abi_version` 不自动跟随数据面变化。** 控制面版本号保护控制面；调整 `ProxyRequest` 或核心服务面结构时必须手工递增，否则旧插件会静默读到错位内存。把这一条写进 P1b 与 P3 的检查清单。

## 不在本次范围

- 插件依赖与加载顺序：供应商差异由 Channel 的 `base_url`/`api_key` 表达，是数据不是代码，不需要依赖结构。
- 多版本并存与回滚：更新即替换，迁移失败即禁用。
- 插件沙箱与能力白名单：与「安装即完全信任」前提冲突。
- 数据面的 C ABI 化：流式路径不接受每块数据一次序列化。
