# 核心负责最终扣款与计费持久化

状态：accepted

> **术语修订。** 原文的 `token_details` 已更名为 `usage_details`。理由是本 ADR 自己确立的
> “核心不解析该字段”原则：不同协议的计量单位不是 token（图片按张、音频按秒），以 token
> 命名会把一个刻意保持不透明的字段绑到某一类协议的语义上。字段含义未变。

计费边界分为两层。插件负责理解上游协议，从请求/响应中提取原始 usage，处理协议专属 token 语义和插件自定义的模型 `pricing` JSON，并得到基础金额 `protocol_cost_usd`；核心负责接收该金额，应用 ChannelGroup.price_multiplier，执行最终扣款、核心请求记录写入和聚合持久化。不同协议的 usage 字段路径、最终事件、流式合并、价格 JSON 和 token 到基础金额的规则不进入核心。核心不解析协议 token JSON 或模型价格 JSON，也不要求所有插件共享固定价格字段。

请求明细表使用 `usage_details` 保存上游响应中的完整 token/usage JSON，不再为不同协议分别增加 token 列。未知字段也必须保留。普通响应直接保存最终 usage 对象；流式响应由插件识别最终 usage 事件，必要时在插件内部合并多个 usage 事件。核心不解析 `usage_details`，而是使用运行时的 `protocol_cost_usd` 应用核心倍率、完成扣款，并把 `ChannelGroupMultiplier` 和最终 `usd` 写入请求记录，再写入 `request_totals` 等聚合结构；最终表不单独保存 `protocol_cost_usd`，可以用 `usd / ChannelGroupMultiplier` 反推基础金额。`request_totals` 只保留请求数、最终金额和首 token 延迟等核心通用字段，不保存 input/output/cache 等协议 token 聚合列。查询阶段不临时扫描原始 JSON 计算聚合。

插件不得直接扣用户余额，不得写入核心请求记录；但可以计算运行时 `protocol_cost_usd` 和协议专属价格语义。核心负责应用 ChannelGroup 倍率、得到最终 `usd`，保存倍率快照，扣除用户余额并写入核心请求记录。插件专属配置、业务表和协议原始数据仍由插件负责 schema、迁移、读写和卸载清理。

普通请求和 SSE 都由协议 hook 在完成或明确失败后调用核心提交函数。插件在调用前已经把 `usage_details` 和 `protocol_cost_usd` 写入 `ProxyRequest`；提交函数只应用核心倍率、完成最终扣款并持久化。核心不根据 hook 返回时机猜测流是否结束，也不扫描、解析或合并 SSE usage。

请求记录的创建不依赖 usage 存在。上游没有 usage，或 SSE 在协议终端事件前中断时，核心仍保存请求记录，保留已捕获的状态码、延迟、响应标识和错误信息；协议计费结果按零处理，不扣除用户余额，`usd` 写为 0。若已经捕获部分原始 usage，仍由插件原样放入 `usage_details`，但该次中断请求不计费。

`requests` 是客户端请求级表，不是 Channel 尝试级表。一次请求在协议插件内部循环切换多个 Channel 时，只在最终结果确定后提交一条核心请求记录；中间失败不单独落库，也不产生独立扣费。插件不因完整一轮 Channel 失败就结束请求，而是回到第一个候选继续尝试；当前设计不新增 `request_attempts` 表或额外的请求-尝试关系。

请求记录的 `channel_id` 表示最终路由归属：请求成功时记录成功的 Channel；请求因客户端断开、取消、不可恢复错误或 Revlm 关闭终止时记录最后一次实际尝试的 Channel，不使用 `0` 伪造“无路由”状态。

请求表只保存核心通用计费字段：`ChannelGroupMultiplier` 和最终 `usd`。旧的 `tier_multiplier`、`service_tier` 属于协议计费语义或协议原始响应，不再作为核心列；原有 `channel_multiplier` 统一改为 `ChannelGroupMultiplier`。协议 tier 信息由插件保留在 `usage_details` 中。

该决定修正此前“所有 usage 字段和计费逻辑都由核心统一承接”的设计：协议计费语义归插件，最终扣款、ChannelGroup 倍率、核心请求记录和写入路径归核心。
