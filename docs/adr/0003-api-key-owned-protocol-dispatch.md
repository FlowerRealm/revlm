# 在用户 API key 归属解析后分发协议请求

状态：superseded by [ADR 0007](0007-registry-replaces-symbol-interposition.md) 与
[ADR 0009](0009-core-owned-failover-loop.md)

> **本 ADR 的分发结论已被 0007 取代。** 仍然有效的部分：分流目标是 API key 解析出的
> ChannelGroup 而不是插件；协议处理是一次完整的边界而不是被核心逐步驱动的虚接口集合；
> 候选 Channel 的迭代状态由核心持有、插件拿不到完整候选列表。已被取代的部分：以
> `LD_PRELOAD` 同名符号链实现分发、`/v1` 由核心独占入口、以及“插件的本质是核心 hpp 的
> cpp 实现”这一前提。
>
> **失败切换与提交的归属已被 0009 取代。** 本文把候选轮转的 `while` 和提交调用都放在插件
> 里；0009 把它们移回核心，插件改为每次尝试返回一个裁决。核心持有迭代状态这一条没变，变的
> 是谁写循环。
>
> 下文保留原文以记录当时的推理。

原方案曾决定禁止插件注册全局 `server.Post` 路由，改用统一插件处理器；该方案后来被用户否决，因为插件必须保留自行注册端点的能力。随后提出的 `for_channel_type()`、逐个插件试探 handler 等对象化路由模型也不符合插件本质，均不采用。

插件的本质是核心 `.hpp` 的 `.cpp` 实现：核心调用普通函数入口，插件补充未实现函数、覆盖已实现函数，或通过 `RTLD_NEXT` 链接原实现。插件不作为需要逐请求发现、选择或试探的运行时对象。

`revlm_register_http_routes(Server&)` 只保留为启动期一次性显式调用例外。插件可以通过它注册普通的全局 Server 端点；`channel` 插件也可以注册这些普通端点，非 `channel` 插件同样只能走这条普通全局路径。

`/v1/*` 是核心拥有的特殊数据面入口。插件不得把相同的 `/v1` 路径无条件注册到所有插件共享的全局 Server：核心先认证 API key，解析到具体 ChannelGroup，再调用可被插件 cpp 实现或覆盖的 hpp hook。同一个 channel 插件的实现可以自然复用处理多个 ChannelGroup。路径本身不承担插件选择职责。

## 同名 hook 分流

`/v1` 的核心入口只调用一个稳定的 hpp 函数，例如：

```cpp
extern "C" void revlm_handle_v1(
    const httplib::Request &, httplib::Response &, ProxyRequest &);
```

该 ABI 直接复用现有 `httplib::Request`、`httplib::Response` 和可变 `ProxyRequest`，不另造 `RequestContext`、`ResponseWriter` 或函数表。插件与当前 Revlm/httplib ABI 绑定，版本更新通过外部冷启动生效；这是完全信任、无多版本共存设计下有意接受的耦合。

`ProxyRequest` 携带核心完成鉴权后得到的具体 ChannelGroup 快照。每个 channel 插件都可以提供这个同名 cpp 实现：

1. 检查 `ChannelGroup.type` 是否属于自己；
2. 不属于自己时通过 `RTLD_NEXT` 调用下一层；
3. 属于自己时处理请求，不再向下转发。

宿主只调用一次符号，不维护插件路由表，也不循环试探多个插件。每个启用的 channel 插件会进入一次 hook，但非目标插件只执行一次上下文判断。`plugin.type` 只表示插件类别，不参与协议选择。

这个 hook 是完整协议边界，而不是一个让核心逐步驱动插件对象的虚接口集合。核心提供鉴权后的 `ProxyRequest`/ChannelGroup 上下文、通用上游传输、流写出、最终扣费和核心请求记录能力；目标插件在一次 hook 调用中自行完成请求解析、协议路径分支、上游请求构造、响应或 SSE 解析、原始 usage 提取以及协议计费结果计算。usage 的最终事件识别、协议字段路径、多事件合并和 token 到协议计费结果的规则也完全属于目标插件。核心不解析协议 token JSON。插件可以在 cpp 内部使用自己的辅助类，但这些类不进入宿主的插件路由或生命周期模型。

进入 hook 前，核心根据 ChannelGroup 状态、成员 Channel 状态、优先级和轮询规则选择一个具体 Channel，并把它放入请求上下文。协议插件只处理这个选中目标；Channel 的通用可用性属于核心路由职责，不再由插件的 `channel_ok()` 或已删除的 `Channel.type` 判断。候选 Channel 的迭代由核心通过一个普通函数（例如 `revlm_next_candidate(ProxyRequest&)`）逐个返回，迭代状态由核心持有：插件每次上游失败后调用它取下一个候选，序列走到末尾后由该函数回到第一个。插件不持有完整候选列表，也没有越界遍历的能力。

如果一次上游尝试失败，是否切换到下一个候选 Channel 属于完整协议 hook：插件理解自己的 4xx/5xx、传输错误和流状态，判断错误是否可恢复。网络错误、timeout 和可恢复的上游 5xx 可以继续轮转；确定性的鉴权、参数或协议错误由插件终止请求。核心不根据通用状态码替插件决定重试。候选 Channel 按既定顺序迭代，走到末尾后回到第一个继续尝试，不设置重试次数上限，也不因完整一轮失败就向客户端返回错误。每次上游尝试仍受单次 timeout 约束；只有上游成功、客户端断开、请求取消、不可恢复错误或 Revlm 关闭时才终止。一旦流已经向客户端发送数据，插件不得切换 Channel。

请求收尾统一由核心提交函数完成，例如：

```cpp
void revlm_commit_request(ProxyRequest &proxy);
```

插件在上游成功、客户端断开、请求取消、不可恢复错误或 Revlm 关闭等最终结果确定后显式调用它。插件先按自身协议确定最终 usage，计算基础金额 `protocol_cost_usd`，将完整原始 `token_details`、`protocol_cost_usd`、status、latency 和最终路由 Channel 写入 `ProxyRequest`；如果协议把 usage 拆在多个事件中，由插件在内部完成合并。一次客户端请求可以在 hook 内循环尝试多个候选 Channel，但只有最终请求结果提交一次核心记录；中间尝试不生成独立请求记录，也不独立计费。成功时核心记录成功 Channel；因中断或不可恢复错误终止时核心记录最后一次实际尝试的 Channel，不用 `0` 替代。没有 usage 或因中断终止时，核心记录请求但不扣费，`protocol_cost_usd` 和 `usd` 均为 0。核心提交函数只负责应用 ChannelGroup 倍率、最终扣款和保存核心请求记录，不解析协议 token JSON、不递归扫描 `usage`、不猜测最终事件，也不合并协议事件。普通请求和 SSE 使用同一条提交路径。

这不是 C++ overload，而是动态链接符号 interposition。函数必须使用稳定的 `extern "C"` ABI，且参数必须包含已经解析好的请求上下文。纯符号链的代价是同一个 `ChannelGroup.type` 若被多个插件声明，结果由插件加载顺序决定；宿主不替插件做冲突校验。

## 已否决的方案

- 由 Revlm 为插件分配端点名称，或禁止插件自行通过 Server 注册端点。
- 使用私有 PluginServer 替代插件可见的全局 Server。
- 把同路径请求交给多个插件依次试探、用 `for_channel_type()` 声明协议或由 handler 返回 false 的方案，错误地把插件当成分流目标；实际分流目标是 API key 所属的具体 ChannelGroup，插件只是被 LD_PRELOAD 调用的 cpp 实现。

## 硬约束

共享 `httplib::Server` 无法按 API key 在同一路径的多个 handler 之间选择；因此 `/v1` 必须只有核心拥有的唯一入口。

同名符号本身也不会按 API key 自动选择实现；分流信息必须进入 hook 参数，由当前实现判断是否处理。若要求只有目标插件被调用，就必须另建启动期函数表，这不再是单纯的 hpp/cpp 覆盖模型。

## 运行时边界

启动阶段允许核心显式调用插件的注册函数一次，用于把普通控制面或管理端点接入全局 Server。进入请求处理后，`/v1` 只有核心入口；核心完成 API key 到具体 ChannelGroup 的解析后，沿普通 hpp hook/LD_PRELOAD 链进入插件实现。

这样既保留插件自行注册普通端点的能力，也避免多个插件在共享 `httplib::Server` 中注册同一个 `/v1` 路径后发生先注册者吞请求的问题。

参考实现：

- [libfaketime 的 `RTLD_NEXT` 包装链](https://github.com/wolfcw/libfaketime/blob/master/src/libfaketime.c#L2640-L2663)
- [NGINX 的启动期 phase handler 链](https://github.com/nginx/nginx/blob/master/src/http/ngx_http.c#L350-L400)
- [Apache 的 hook 链定义](https://github.com/apache/httpd/blob/trunk/server/config.c#L71-L105)
- [OpenSSL 的函数 dispatch table](https://github.com/openssl/openssl/blob/master/include/openssl/core_dispatch.h#L31-L62)
