# 数据面插件与 Gateway

Revlm 的插件不是一层被宿主“允许”的回调 API。它们是受信任的 Linux 共享库：启动 worker 前，
bootstrap 把启用模块写入 `LD_PRELOAD`，动态链接器让插件中同名的普通 C++ 符号优先于
`librevlm_core`。因此插件要改哪个已导出、可插桩的函数，由插件自己决定。

没有 `Plugin` factory、manager、registry、`HostServices`、固定路由登记表或声明式前端 schema。
包管理只负责文件、migrations、依赖和启动顺序；它不知道模块替换了什么。

## 运行模型

```text
/revlm bootstrap
  ├─ 初始化核心 schema、执行插件 migrations
  ├─ 解析启用包和 preload 顺序
  ├─ LD_PRELOAD="libOpenAI.so libAnthropic.so ..."
  └─ exec /revlm-worker
       └─ 动态链接器先解析插件符号，再启动正常 HTTP 服务
```

官方模块分别定义普通的 `revlm_models_for_channel_type`、`revlm_prepare_upstream`、
`revlm_retry_upstream_request` 与 `revlm_register_http_routes` 函数：同一个模块拥有自己的模型、
路由、上下游转换、SSE 和用量解析。它可以选择调用 `RTLD_NEXT` 继续链，也可以直接替换
`UpstreamExecutor::prepare`、整个 HTTP 安装函数或其他公开符号。这个机制没有“授权列表”；安装插件
本来就等价于让它在进程内任意执行代码。

`LD_PRELOAD` 只适用于 Linux ELF。已内联、`static`、隐藏可见性或不经动态符号解析的调用无法被
拦截，这属于工具链事实，而不是 Revlm 的限制。运行中 `dlopen` 无法可靠重绑已加载库的调用，
所以安装、启停、卸载都必须重启 worker。

## OpenAI 与 Anthropic

首批系统插件为 `OpenAI`、`Anthropic`：

| 插件 | 覆盖的普通实现 | 对外协议 |
| --- | --- | --- |
| `OpenAI` | 自己的模型目录、Chat/Responses route、上游认证与重试逻辑 | `/v1/chat/completions`、`/v1/responses`、`/v1/responses/input_tokens` |
| `Anthropic` | 自己的模型目录、Messages route、上游认证和 SSE/用量逻辑 | `/v1/messages` |

现有渠道类型字符串 `openai_compatible`、`anthropic` 保持不变，旧数据库行无需迁移。核心保留普通
`/v1/models` 与 `/v1/models/:model_id`：它先按 token 找渠道组，再用该组唯一的插件类型请求模型。
渠道组不能混入两种插件类型。没有对应协议插件时，其协议路由根本不会注册，稳定返回 404。

`Gateway` 和 `v1_http` 都只是普通可复用代码，不是插件 SDK。官方插件用它们复用现有 token 鉴权、
渠道选择、上游传输、SSE 泵送和用量提交；第三方插件可以使用、覆盖或完全绕开它。

## 前端与渠道

核心前端只保留通用渠道编辑器：`type`、已有通用列与原始 `config_json`。每个包可选带
`frontend/entry.js` 及任意同目录资源；浏览器启动控制台后动态导入 entry。该 JavaScript 是任意
ESM，可以创建自己的 React 树、替换页面、修改路由或网络请求。没有字段 schema 或组件接口。资产
服务使用 worker 启动时记录的包快照，因此上传或停用不会偷偷变成前端热加载。

## 冲突与测试

同一符号有多个插件定义时，`LD_PRELOAD` 中靠前模块获胜。包的 `load_order` 越大越靠前，依赖包
排在依赖者后；需要协作覆盖时，插件作者自行通过明确顺序或 `RTLD_NEXT` 链式调用处理。核心不会
合并、注册或拒绝这类替换。

Linux CI 用真实 `LD_PRELOAD` probe 验证符号替换，并让数据面回归测试带官方模块运行。macOS 的
Mach-O 两级绑定不提供等价行为，因此本地 macOS 构建只能验证编译，不能证明 Linux 替换链路。
完整包格式与构建方式见 [`revlm-plugin`](https://github.com/FlowerRealm/revlm-plugin)。
