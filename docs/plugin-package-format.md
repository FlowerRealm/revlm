# Revlm V1 插件包与 SDK 合同

当前插件合同跟随 `FlowerRealm/revlm-plugin` 上游 main（子模块提交
`6050bc952b95d6561a220f0738b3a9298ce29d3d`）。旧的 `format_version: 2` ELF preload 合同已移除。

## 包布局

`.revlm-plugin` 是 ZIP，生产上传包包含：

```text
plugin.json
backend/linux-amd64/libExample.so
backend/linux-arm64/libExample.so
frontend/channel-types.json
migrations/0001_initial.sql       # 可选，只进不退
```

宿主拒绝路径遍历、绝对路径、反斜杠、符号链接、加密条目和 ZIP64，并在 staging 完成校验后原子发布。

## manifest

```json
{
  "format_version": 1,
  "id": "Example",
  "name": "Example",
  "version": "1.0.0",
  "sdk_abi": "revlm-plugin-cpp-v1",
  "requires": [],
  "targets": [
    { "os": "linux", "arch": "amd64", "module": "backend/linux-amd64/libExample.so" },
    { "os": "linux", "arch": "arm64", "module": "backend/linux-arm64/libExample.so" }
  ],
  "frontend_schema": "frontend/channel-types.json",
  "migrations": []
}
```

`id`、`version`、依赖 ID 和相对路径必须安全；每个平台只能有一个模块。
`sdk_abi` 必须精确等于 `revlm-plugin-cpp-v1`。包内 schema 必须是包含 `channel_types` 数组的 JSON，且
每个插件注册的渠道类型都必须在 schema 中声明。manifest 的 migrations 必须与模块注册的列表完全一致。

## SDK 加载生命周期

worker 对 bootstrap 传来的不可变包快照执行：

1. `dlopen(module, RTLD_NOW | RTLD_LOCAL)`；
2. 查找 `revlm_plugin_create_v1()` 与 `revlm_plugin_destroy_v1()`；
3. 创建插件并调用 `Plugin::register_with(PluginRegistrar&)`；
4. 校验只允许 `GET`/`POST` 的 `/v1/*` 路由、非空 handler、无重复路由/渠道类型；
5. 校验 manifest migrations 与注册列表、包内 channel schema；
6. 校验通过后执行 migrations，并将路由和渠道类型并入冻结 registry；
7. worker 结束时先 destroy，再 `dlclose`。

插件只能注册数据面路由、渠道类型和 migrations。核心继续负责 token 鉴权、请求限制、余额、上游调度、SSE、
用量和计费。普通请求由核心提交一次用量；流式请求由流结束回调提交。

## 前端 schema

V1 不执行插件 JavaScript。`frontend/channel-types.json` 描述 `text`、`secret`、`number`、`boolean`、`select`
字段。`base_url`、`api_key`、`price_multiplier` 可绑定核心渠道列；其他字段写入 `config_json`。
核心前端通过 `GET /api/plugins/channel-types` 聚合 active 插件 schema。

## 启停与卸载

上传、启用、停用和卸载都要重启才生效。卸载不执行 down migration，也不删除插件业务数据；系统包只能停用。
插件是受信任本机代码，V1 不提供签名、沙箱或权限隔离。
