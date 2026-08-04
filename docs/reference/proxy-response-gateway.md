# 数据面插件与 /v1 协议 hook

Revlm 的插件不是一层被宿主“允许”的回调 API。它们是受信任的 Linux 共享库：启动 worker 前，
bootstrap 把启用模块写入 `LD_PRELOAD`，动态链接器让插件中同名的普通 C++ 符号优先于
`librevlm_core`。因此插件要改哪个已导出、可插桩的函数，由插件自己决定。

没有 `Plugin` factory、manager、registry、`HostServices`、固定路由登记表或声明式前端 schema，
也不存在驱动插件的 `Gateway` 虚基类模型。包管理只负责文件、生命周期符号和启动顺序；
它不知道模块替换了什么。安装真相在文件系统：`active/`、`disabled/`、`pending/`、`failed/`、
`backups/` 标记表达，没有 `plugin_installations` / `plugin_migrations` 表。

## 运行模型

```text
/revlm bootstrap
  ├─ 初始化核心 schema、调用启用插件的迁移符号
  ├─ 解析启用包和 LD_PRELOAD 顺序（按插件 id 字典序）
  ├─ LD_PRELOAD="libOpenAI.so libAnthropic.so ..."
  └─ exec /revlm-worker
       └─ 动态链接器先解析插件符号，再启动正常 HTTP 服务
```

官方模块分别定义普通的 `revlm_handle_v1`（完整协议 hook）、`revlm_models_for_channel_type` /
`revlm_all_models`（模型目录）与 `revlm_prepare_upstream`（上游准备）函数。核心拥有的唯一
`/v1` 数据面入口是 `revlm_handle_v1`：核心先解析用户 API key 的归属 ChannelGroup，再调用
同名 hook 链；插件检查 `ChannelGroup.type` 是否属于自己，不是则沿 `dlsym(RTLD_NEXT, ...)`
继续。未命中任何启用协议插件时，核心默认实现返回 500，不写请求记录、不扣费。插件在同一
hook 调用内完成协议端点分支（含 `/v1/models`）、上游请求构造、响应或 SSE 解析、原始 usage
提取与协议计费结果；核心提供鉴权后的 ChannelGroup 快照、通用上游传输、候选迭代与最终提交。
这个机制没有“授权列表”；安装插件本来就等价于让它在进程内任意执行代码。

`LD_PRELOAD` 只适用于 Linux ELF。已内联、`static`、隐藏可见性或不经动态符号解析的调用无法被
拦截，这属于工具链事实，而不是 Revlm 的限制。运行中 `dlopen` 无法可靠重绑已加载库的调用，
所以安装、启停、卸载都必须重启 worker。

## OpenAI 与 Anthropic

首批系统插件为 `OpenAI`、`Anthropic`：

| 插件 | 处理的 ChannelGroup.type | 对外协议 |
| --- | --- | --- |
| `OpenAI` | `openai_compatible` | `/v1/chat/completions`、`/v1/responses`、`/v1/responses/input_tokens`、`/v1/models` |
| `Anthropic` | `anthropic` | `/v1/messages`、`/v1/models` |

渠道类型字符串 `openai_compatible`、`anthropic` 保持不变，旧数据库行无需迁移。协议类型与
价格倍率是 ChannelGroup 的属性（`ChannelGroup.type`、`ChannelGroup.price_multiplier`），
channel 只保留 `id/name/status/priority/base_url/api_key`。插件按 `ChannelGroup.type` 决定
是否处理请求，并拥有该类型的模型目录；核心只按 `ChannelGroup.type` 索引与转发，不解析价格
JSON。`/v1/models` 是插件完整协议 hook 的端点分支，不在核心数据面。没有对应协议插件时，
其协议路由不会注册到核心，稳定返回 500 且不扣费。v3 不再要求一个渠道组只能包含一种插件
类型：协议分发由组属性 `type` 驱动，多个插件命中同一 type 时直接遵循动态链接器的符号优先级。

`Gateway`（`gateway.cpp`/`gateway.hpp`）只是核心内部保留的传输与计费工具实现细节，不是插件
SDK，也不是虚基类驱动的插件模型：v3 插件通过 `revlm_next_candidate` 迭代候选 Channel、
调用核心通用上游传输执行请求，并在最终提交时给出原始 `token_details` 与运行时基础金额
`protocol_cost_usd`。第三方插件可以使用、覆盖或完全绕开核心的传输工具。

## 计费与提交

一次客户端请求只提交一次核心请求记录。插件在最终结果确定后填入 `ProxyRequest.token_details`
（完整原始 token/usage JSON，未知字段保留）与 `protocol_cost_usd`（运行时基础金额），再调用
`revlm_commit_request`；核心应用 `ChannelGroup.price_multiplier`、扣款并持久化 `usd` 与
`channel_group_multiplier` 快照，从不解析协议 token JSON。流式响应在 SSE 泵内扫描最终 usage
事件（必要时由插件合并）；一次 hook 内中间 Channel 尝试不落库也不独立计费。核心不维护
Gateway 虚基类、也不把输入/输出/cache 拆成固定字段。

## 前端与渠道

核心前端只保留通用渠道编辑器：channel 的 `id/name/status/priority/base_url/api_key` 与
ChannelGroup 的 `type/price_multiplier`。每个包必须提供 `frontend/entry.js` 及任意同目录
资源；浏览器启动控制台后动态导入 entry。该 JavaScript 是任意 ESM，可以创建自己的 React 树、
替换页面、修改路由或网络请求。没有字段 schema 或组件接口。资产服务使用 worker 启动时记录的
包快照，因此上传或停用不会偷偷变成前端热加载。

## 冲突与测试

已启用插件按插件 `id` 字典序进入 `LD_PRELOAD`；同一符号有多个插件定义时，字典序靠前的模块
获胜。多个插件命中同一 `ChannelGroup.type` 时核心不做冲突检测，直接遵循动态链接器的符号
优先级；需要协作覆盖时，插件作者自行通过明确顺序或 `RTLD_NEXT` 链式调用处理。核心不会
合并、注册或拒绝这类替换。

Linux CI 用真实 `LD_PRELOAD` probe 验证符号替换，并让数据面回归测试带官方模块运行。macOS 的
Mach-O 两级绑定不提供等价行为，因此本地 macOS 构建只能验证编译，不能证明 Linux 替换链路。
完整包格式与构建方式见 [`revlm-plugin`](https://github.com/FlowerRealm/revlm-plugin)。
