# 插件架构 Grill 决策账本

截至 2026-08-02。本文记录本次设计访谈的统计口径和已经确认的结论，不替代 `CONTEXT.md` 或 ADR。

## 统计口径

按“一个相对独立的设计主题”统计，不按每条“接受”消息重复计数；同一主题下同时删除多个字段，计为一个主题。

  - 已确认决策主题：81 个
  - 既有访谈整理：35 个
  - 上一轮整理后新增确认：17 个
  - 本轮新增：3 个（`/v1` 特殊边界、插件类别字段、channel 插件保留普通全局端点能力）
  - 后续新增：1 个（channel 插件进入专用路由集合）
  - 再后续新增：1 个（`ChannelGroup` 快照作为 `/v1` handler 上下文）
  - 再后续新增：1 个（启动期 Server 注册例外与核心唯一 `/v1` 入口）
  - 本次新增：1 个（同名 hpp hook 通过 `RTLD_NEXT` 链按 ChannelGroup.type 分流）
  - 本次新增：1 个（`/v1` 由完整协议 hook 处理，核心不再用 Gateway 虚接口逐步驱动插件）
  - 本次新增：1 个（协议 hook 直接使用 `httplib::Request`、`httplib::Response` 和 `ProxyRequest`，不新增包装 ABI）
  - 本次新增：1 个（核心在进入协议 hook 前按通用字段选择具体 Channel）
  - 本次新增：1 个（失败切换策略由完整协议 hook 决定，核心只提供候选 Channel 迭代）
  - 本次新增：1 个（普通请求与 SSE 均由协议 hook 显式调用核心提交函数完成收尾）
  - 本次新增：1 个（最终扣款、倍率与核心请求数据库写入归 Revlm，插件不直接操作核心账务）
  - 本次新增：1 个（请求明细使用 `token_details` 原样保存完整 token/usage JSON，聚合表保留核心统计列）
  - 本次新增：1 个（协议插件负责最终 usage 事件识别、字段提取和流式合并，核心不扫描或解析协议 usage）
  - 本次新增：1 个（无 usage 或 SSE 中断也写请求记录，计费为 0）
  - 本次新增：1 个（`requests` 按客户端请求记录，一次请求多 Channel 尝试只提交一次）
  - 本次新增：1 个（Channel 候选失败后无限循环尝试，受单次 timeout 和外部取消/关闭终止）
  - 本次新增：1 个（请求终止时 `channel_id` 记录成功或最后一次实际尝试的 Channel）
  - 本次新增：1 个（失败可恢复性由协议插件判断，核心不按通用状态码重试）
  - 本次新增：1 个（协议 token 解析和协议计费语义归插件，核心只负责最终扣款、倍率和持久化）
  - 本次新增：1 个（插件提交 `token_details` 与 `protocol_cost_usd`，核心应用倍率、扣款和写库）
  - 本次新增：1 个（`request_totals` 删除协议 token 聚合列，只保留核心通用聚合字段）
  - 本次新增：1 个（不持久化 `protocol_cost_usd`，请求表保存 `ChannelGroupMultiplier` 和最终 `usd`）
  - 本次新增：1 个（删除 `tier_multiplier`、`service_tier`，`channel_multiplier` 改为 `ChannelGroupMultiplier`）
  - 本次新增：1 个（模型目录由插件管理；`Model` 删除 `owned_by`、`icon_url`，价格字段保留）
  - 本次新增：1 个（模型价格不再固定字段，改为插件自定义 `pricing` JSON）
  - 本次新增：1 个（共享 `Model` ABI 只保留 `id`、`name`、`pricing`）
  - 本次新增：1 个（`Model.id` 只在插件的 ChannelGroup.type 作用域内有效）
- 明确否决的提案：5 个
  - 插件迁移在核心数据库之前执行
  - ChannelGroup.type 不允许修改
  - 由 Revlm 统一分配插件 API 端点、禁止插件通过 Server 注册端点
  - 使用私有 PluginServer 替代插件可见的全局 Server
  - 让多个插件依次试探同一路径并用 bool 返回值决定是否处理
- 用户主动修正或收紧定义：至少 15 次
  - type 属于 ChannelGroup 而不是 Channel
  - owned_by、icon_url 不进入 Model；模型目录由插件管理，价格改为自定义 JSON
  - multiplier 只属于 ChannelGroup
  - 版本目录删除但版本字段保留
  - 分发依据是 API key 归属而不是 URL 类型
  - 只有 `/v1/*` 需要特殊分发，其他插件端点继续使用全局 Server
  - 新增 plugin.type 区分插件类别
  - `channel` 插件通过核心唯一 `/v1` 入口进入特殊处理，同时可以注册普通全局端点
  - ChannelGroup.type 修改不联动 Channel
  - 分流目标是 API key 所属的具体 ChannelGroup，同一插件可复用处理多个 ChannelGroup
  - 插件本质是 hpp hook 的 cpp 实现，不应被对象化路由系统主动逐请求调度
  - `revlm_register_http_routes(Server&)` 只在启动期显式调用；`/v1` 由核心唯一入口分发到 ChannelGroup 后进入 hpp hook
  - 同一个 hpp hook 由多个插件提供同名实现；非目标插件通过 `RTLD_NEXT` 转发，宿主不维护插件候选列表
  - 最终扣款、ChannelGroup 倍率和核心请求写入属于核心；协议 token 解析和协议计费语义属于插件
  - token 明细不再拆成协议专属列，改为 `token_details` 原样 JSON；聚合统计仍由核心维护
- 本次访谈期间代码改动：0
- 已新增文档：1 个 glossary、4 个 ADR、1 个决策账本

## 已确认的主线

1. 插件是完全信任的原生前后端扩展；无沙箱、签名、依赖和多版本回滚。
2. 清单只保留 id、type、name、description、version；后端和前端入口都必须提供但可以是 no-op/空实现。
3. 插件包单目录覆盖安装；版本保留为元数据；启用状态、卸载待处理状态由文件系统表示；变更等待外部冷启动。
4. 插件数据库由插件自行管理；核心 schema 之后执行插件迁移，卸载执行插件清理。
5. ChannelGroup 拥有 type 和 price_multiplier；Channel 不拥有 type、multiplier、config_json 或模型列表。
6. 最终扣款、ChannelGroup 倍率和核心请求记录写入是核心功能；插件负责协议 token 解析和协议计费语义，核心不解析协议 token JSON。
7. 模型目录按 ChannelGroup.type 查询插件并由插件管理；Model 只保留模型身份和插件自定义 `pricing` JSON，删除 `owned_by`、`icon_url`，核心不拥有或解析模型目录。
8. 用户 API key 先解析到 ChannelGroup；URL 不承担插件选择职责。
9. 插件可以在启动期通过 `revlm_register_http_routes(Server&)` 自行注册普通全局 Server 端点；`/v1` 不进入共享全局路由表。
10. 新增 `plugin.type` 作为插件类别；`plugin.type == "channel"` 进入 `/v1` 特殊处理，不能与 `ChannelGroup.type` 混用。
11. `channel` 插件同时可以注册普通全局 Server 端点；其他类别只走普通全局 Server。
12. `/v1` 核心入口只调用一个稳定 hpp hook；每个 channel 插件按已解析的 `ChannelGroup.type` 决定处理或 `RTLD_NEXT` 转发。`plugin.type` 不参与协议分流。
13. 完整协议 hook 在一次调用内负责协议请求解析、上游转换、响应/SSE、原始 usage 提取和协议计费结果计算；核心负责最终扣款、核心记录和倍率应用，不解析协议 token JSON，也不持有插件 Gateway 对象。
14. 协议 hook ABI 固定为 `revlm_handle_v1(const httplib::Request &, httplib::Response &, ProxyRequest &)`；不增加 `RequestContext`、`ResponseWriter` 或函数表，插件随当前 Revlm/httplib ABI 冷启动升级。
15. 核心在进入协议 hook 前根据 ChannelGroup/Channel 通用字段选择具体 Channel；插件只处理选中目标，不再通过 `channel_ok()` 或 `Channel.type` 判断可用性。
16. 上游失败后的 Channel 切换由完整协议 hook 按协议语义决定；核心只提供候选顺序和迭代能力，不根据通用 HTTP 状态码自动重试，流开始后禁止切换。
17. 普通响应和 SSE 都由完整协议 hook 在完成或明确失败后显式调用核心提交函数；插件提交原始 `token_details` 和 `protocol_cost_usd`，核心应用 ChannelGroup 倍率、完成最终扣款并保存核心请求记录，不根据 hook 返回时机猜测流状态。
18. 插件专属数据库仍由插件负责 schema、迁移、业务数据和卸载清理，但核心请求记录及其计费写入始终由核心完成。
19. `requests` 明细表用 `token_details` 保存完整原始 token/usage JSON；协议 token 标量列删除，`request_totals` 只保留请求数、最终金额和首 token 延迟等核心通用聚合字段。
20. usage 的最终事件识别、协议字段路径、流式多事件合并和协议计费结果由协议插件负责；插件在提交前写入 `token_details` 和 `protocol_cost_usd`，核心不递归扫描、按协议解析或合并 SSE usage。
21. 请求记录创建不依赖 usage 存在；无 usage 或 SSE 在终端事件前中断时仍落库，保留状态、延迟和错误信息，`usd` 为 0 且不扣费；已捕获的部分 `token_details` 可以保留。
22. `requests` 是客户端请求级记录；一次请求内部可以尝试多个 Channel，但只提交一条核心记录，中间尝试不独立落库、不独立计费，也不新增 `request_attempts` 表。
23. Channel 候选按顺序循环，走到末尾后回到第一个继续尝试；不设置重试次数上限，不因一轮失败返回错误。每次尝试受单次 timeout 约束，最终由成功、客户端断开、请求取消、不可恢复错误或 Revlm 关闭终止。
24. `requests.channel_id` 成功时记录成功 Channel；请求因客户端断开、取消、不可恢复错误或 Revlm 关闭终止时记录最后一次实际尝试的 Channel，不用 `0` 作为失败兜底。
25. 失败是否可恢复由协议插件判断；网络错误、timeout 和可恢复 5xx 可以继续轮转，确定性的鉴权、参数或协议错误终止请求。核心不维护通用状态码重试策略。
26. 协议 token 解析、usage 事件处理和协议计费语义全部由插件完成；核心只接收协议计费结果，应用 ChannelGroup 倍率、完成最终扣款并写入核心记录，不解析协议 token JSON。
27. 插件向核心提交原始 `token_details` 和运行时基础金额 `protocol_cost_usd`；核心计算最终 `usd = protocol_cost_usd * ChannelGroup.price_multiplier`，执行扣款并写入核心记录。
28. 核心不保存或聚合固定的 input/output/cache token 列；协议 token 统计由插件或原始 `token_details` 承担，核心聚合只保留通用指标。
29. `protocol_cost_usd` 不进入最终数据库；请求记录保存 `ChannelGroupMultiplier` 和最终 `usd`，基础金额可由两者反推。
30. 请求表不保存 `tier_multiplier`、`service_tier` 等协议计费字段；原有 `channel_multiplier` 改为 `ChannelGroupMultiplier`，协议 tier 信息保留在 `token_details`。
31. 模型目录和价格由插件管理；共享 `Model` 结构使用插件自定义 `pricing` JSON，删除固定价格字段、`owned_by` 与 `icon_url`，核心只按 ChannelGroup.type 索引和转发。
32. 共享 `Model` ABI 的最小字段固定为 `id`、`name`、`pricing`；插件前端资源自行管理图标等展示资产。
33. `Model.id` 只在插件自己的 ChannelGroup.type 作用域内有效；核心不做跨插件全局唯一校验，索引使用协议类型加模型身份。

## 已确认的运行时边界

`revlm_register_http_routes(Server&)` 是启动期一次性显式调用例外。普通非 `/v1` 端点由插件自行注册；`/v1` 由核心统一入口完成 API key 到具体 ChannelGroup 的解析，再通过同名 hpp hook/LD_PRELOAD 链进入插件实现。插件不参与请求级对象发现、候选试探或按插件 ID 分流。

## 2026-08-03 grill 追加

本轮先确认术语：**plugins v3** 是整套插件产品架构的代号；当前 `.revlm-plugin` 包契约仍称**包格式 v1**，不把产品架构代号误写成包格式版本。

本轮 grill 钉死插件包格式 v1（完整契约见 [`plugin-package-format.md`](./plugin-package-format.md)）与若干数据面边界：

- **清单五字段**：`id`/`type`/`name`/`description`/`version`，无 `format_version`；`core_abi`/`requires`/`targets`/`load_order`/`migrations` 全部删除。此前没有已发布的格式版本，内部迭代的旧格式记录不属于产品状态，格式即当前 v1，清单不携带自版本字段。
- **目录约定即声明**：后端入口是 `backend/<amd|arm>/`（文件名任意，当前平台缺失即报错）；前端入口是强制 `frontend/entry.js`（可为空/no-op）。两者都不进清单。
- **普通路由冲突**：插件与核心或插件之间注册同一非 `/v1` 路径时，不做冲突检测、不强制路径前缀，直接接受 `httplib::Server` 的注册顺序语义，后果由插件自行承担。
- **卸载清理重试**：cleanup 失败不自动重试；用户再次显式发起卸载时重新调用 cleanup，成功后删除包，失败继续保留并标记 `failed`。
- **生命周期数据库访问**：无参数生命周期符号需要数据库时，插件直接调用核心公开的全局数据库访问符号；不向 ABI 传入 `odb::database&`，也不由插件自建第二套连接。
- **卸载清理失败**：保留包、标记 `failed`、不自动重试，用户显式重新触发卸载。
- **加载顺序**：按插件 `id` 字典序进入 LD_PRELOAD；同名符号先者赢，host 不校验冲突。
- **无 ABI 校验**：`core_abi` 彻底删除，host 不比对任何版本标记，错配由用户负责。
- **提交失败**：`revlm_commit_request` 扣款或写库失败时，响应尚未写出则返回 HTTP 500；HTTP/SSE 已开始发送时不能改写，只结束连接，不重试上游。
- **迁移失败**：核心 schema 完成后，只要任一启用插件的 `revlm_plugin_migrate()` 失败，bootstrap 都记录失败原因并拒绝启动 worker；禁用插件不执行迁移，因此不以其迁移状态阻止启动。
