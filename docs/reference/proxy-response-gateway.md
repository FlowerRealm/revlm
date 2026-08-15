# 数据面插件与协议分发

Revlm 的插件是受信任的原生共享库。宿主在启动时 `dlopen` 每个启用的包并调用它的注册入口，插件把
自己的能力登记到核心的两张注册表：路由表和模型目录。插件不覆盖核心已有实现，也不依赖动态链接器
的符号优先级——上一版依赖 `LD_PRELOAD` 的符号插桩已经退役（[ADR 0006](../adr/0006-lifecycle-symbols-are-loaded-explicitly.md)、
[ADR 0007](../adr/0007-registry-replaces-symbol-interposition.md)），它在 macOS 的两级命名空间下根本
不成立，等于让整个数据面只在 Linux 上真正工作过。

安装插件仍然等于无条件信任它：注册之后，插件直接使用核心的 C++ 数据面类型，没有沙箱，也没有能力
白名单。变的只是它"怎么被找到"，不是"它能做什么"。

## 运行模型

```text
/revlm
  ├─ 初始化核心 schema
  ├─ 注册核心路由（/api/*、探针）
  ├─ 对每个启用包：dlopen → 迁移入口 → 注册入口
  │    └─ 插件登记 (方法, 路径, ChannelGroup.type) → 协议处理器，以及按 type 的模型目录
  ├─ 注册无前缀 catch-all
  └─ 开始服务
```

注册顺序是有意义的：`httplib` 按注册顺序匹配，核心路由在前（插件不能遮蔽 `/api/user/login`），
插件的普通全局端点其次，最后才是把剩下的路径交给路由表的 catch-all。

## 一次数据面请求

1. 用用户 API key 解析出它归属的 ChannelGroup。
2. 用 `(方法, 路径, ChannelGroup.type)` 查路由表。查不到就是 HTTP 500，请求不进入任何插件，也不写
   核心请求记录。以 `/` 结尾的注册路径按最长前缀认领它下面的一切，`GET /v1/models/:id` 就是这么服务
   的；核心不解析路径，模型 ID 由插件自己读。
3. 核心持有候选 Channel 的轮转循环。每轮取一个候选、调用一次协议处理器，处理器返回**尝试裁决**：
   `Done` 表示终局，`NextCandidate` 表示这次失败可恢复。可恢复性由插件按协议语义判断，核心不读状态
   码也不读响应体（[ADR 0009](../adr/0009-core-owned-failover-loop.md)）。
4. 轮转是环形的，但有下界：组内每个 Channel 各试一次之后核心停止，返回 HTTP 502 并带上最后一次的
   错误信息。
5. 循环的唯一出口提交一次核心请求记录并扣费。插件没有调用提交的接口，所以重复计费在结构上不可能。

## 流式响应

协议处理器不同步向客户端写字节：HTTP 服务器只在请求处理函数返回之后才交出可写 sink。处理器把状态
码、响应头和一个响应体 pump 交接给核心，核心装载它，并在 pump 结束时提交那一次记录。这个时序本身
就是"流开始后不再切换候选"的机制。

核心交给 pump 的客户端 sink 除了写入还提供存活探测：上游沉默期间只靠写入发现不了已经离开的客户端。
pump 在等待上游的间隙探测客户端，一旦客户端离开就切到短暂的收尾窗口，把上游还能送达的最终 usage
收完就结束，而不是占着上游连接等到空闲超时。SSE 分帧与 usage 提取始终在 pump 内部，核心不解析。

## OpenAI 与 Anthropic

首批系统插件为 `OpenAI`、`Anthropic`：

| 插件 | `ChannelGroup.type` | 登记的路由 |
| --- | --- | --- |
| `OpenAI` | `openai_compatible` | `GET /v1/models`、`GET /v1/models/`、`POST /v1/chat/completions`、`POST /v1/responses`、`POST /v1/responses/input_tokens` |
| `Anthropic` | `anthropic` | `GET /v1/models`、`POST /v1/messages` |

两个 type 字符串与既有渠道数据保持一致，旧数据库行无需迁移。渠道组不能混入两种插件类型。没有对应
协议插件时，那些路径根本不在路由表里，catch-all 直接返回 404。

核心提供给处理器的服务面只有最小集合：上游传输、SSRF 校验、余额预检和流式响应交接。候选轮转与请求
提交不在其中——它们是核心循环自身的行为。

## 前端与渠道

核心前端只保留通用渠道编辑器：`type`、已有通用列与原始 `config_json`。每个包可选带 `frontend/entry.js`
及任意同目录资源；浏览器启动控制台后动态导入 entry。该 JavaScript 是任意 ESM，可以创建自己的 React
树、替换页面、修改路由或网络请求。没有字段 schema 或组件接口。资产服务使用进程启动时记录的包快照，
因此上传不会偷偷变成前端热加载。

## 冲突与测试

两个启用插件登记同一个路由键时，宿主在注册阶段直接报错，后注册的成为失败插件——不按加载顺序静默取
舍。模型目录按插件分开保存，因此停用一个插件只会移除它自己的条目。

测试用真实的包目录：`backend/tests/CMakeLists.txt` 把 OpenAI 与 Anthropic 打成已安装包的样子放进构建
目录，测试进程 `dlopen` 它们，走的是和生产完全相同的路径。这在 macOS 上同样有效，而这正是符号插桩
时代做不到的事。完整包格式与构建方式见 [`revlm-plugin`](https://github.com/FlowerRealm/revlm-plugin)。
