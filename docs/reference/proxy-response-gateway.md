# 数据面插件与 Gateway

Revlm V1 插件是受信任的原生模块，但不再通过 `LD_PRELOAD` 任意替换核心符号。worker 在启动时用
`dlopen(module, RTLD_NOW | RTLD_LOCAL)` 加载模块，调用 `revlm_plugin_create_v1()`，由
`PluginRegistrar` 收集并校验有限的扩展点。

## 允许的扩展点

插件只能注册：

- `/v1/*` 下的 `GET` / `POST` 数据面路由；
- 渠道类型 descriptor，包括模型目录和上游请求准备逻辑；
- 只进不退的 SQL migrations。

核心继续拥有 listener、请求限制、请求 ID、token 鉴权、余额、上游调度与传输、SSE 工具、用量和计费。
重复路由、重复渠道类型、非法方法、空 handler、schema 缺失或 migration 声明不一致都会使该插件失败，
但不会阻止核心管理面启动。

## 请求与计费

插件 handler 收到已经完成 token 鉴权的 `AuthenticatedRequest`、`ResponseWriter` 和 `HostServices`。
普通响应由核心根据 `DataPlaneResult` 最多提交一次用量；流式响应由 SDK Gateway 的终止回调提交一次。
`Gateway` facade 复用渠道选择、上游传输、SSE 泵送和失败切换；协议头、token 语义、响应格式和 usage
解析由插件负责。

## 生命周期

bootstrap 初始化核心 schema、整理包状态并把精确包根路径写入 `REVLM_PLUGIN_ROOTS`，随后 exec worker。
worker 对快照中的每个模块执行 factory、注册、route/channel/schema 全量校验；校验成功后才执行 migration，
再将模块句柄、插件实例和 handler 保存到冻结 registry。worker 停止时先调用 destroy，再 `dlclose`。
上传、启用、停用和卸载都要重启才生效；没有热加载、热卸载、签名、沙箱或权限隔离。

## 官方插件

上游 `FlowerRealm/revlm-plugin` 当前 main 提供：

- `OpenAI`：models、chat completions、Responses 和 input-token 路由；
- `Anthropic`：Messages 路由。

V1 不执行插件 JavaScript。核心通过 `/api/plugins/channel-types` 聚合包内
`frontend/channel-types.json`，绑定字段写入核心渠道列，其他字段写入 `config_json`。
包格式、`sdk_abi`、factory 和 schema 详见 [插件包格式](../plugin-package-format.md)。
