# 插件包格式（v1）

本页是 Revlm 插件包的当前契约，即插件包格式 v1。此前没有任何已发布的格式版本，内部迭代的
旧格式记录（旧 submodule `preload-format.md`、旧 `format_version`）不属于产品状态；格式随文档
迭代，因此清单不携带自版本字段。

## 包内容

上传文件扩展名为 `.revlm-plugin`，内容是 ZIP。生产包必须同时包含 `backend/amd/` 与 `backend/arm/` 两个平台产物；每个平台目录必须恰好包含一个可加载的 `.so`，文件名任意。其他辅助文件可以放在包内，但不能让平台目录出现第二个 `.so`。

```text
plugin.json
backend/amd/libX.so       # 平台目录即声明；文件名任意；当前平台缺失即报错
backend/arm/libX.so
frontend/entry.js          # 必须提供，可为空/no-op ESM
```

- 归档路径必须是相对 POSIX 路径，不含 `..`、绝对路径、符号链接、加密项或 ZIP64；除下列必需入口外，包可以携带任意额外文件，宿主原样保留并由插件自行使用。
- 解压校验大小、CRC 与清单后，内容先写入 staging，再原子发布到
  `REVLM_PLUGIN_DIR/packages/<id>/`（单目录覆盖，无版本目录层级）。
- 每个插件单份存在；再次安装（含更新版本）替换同一目录，不提供用户可见的版本并存或手动回滚。更新前的旧包作为临时备份保留到下一次冷启动完成迁移；若新包迁移失败，host 恢复旧包和旧启用状态，再拒绝启动 worker。

## 清单

`plugin.json` 只有五个字段，无 `format_version`、`core_abi`、`requires`、`targets`、
`load_order`、`migrations`：

```json
{
  "id": "Anthropic",
  "type": "channel",
  "name": "Anthropic",
  "description": "Anthropic Messages 协议插件",
  "version": "0.2.0"
}
```

- `id`、`version` 只含字母、数字、`-`、`_`、`.`。
- `type` 是插件类别，`channel` 表示参与渠道协议数据面；`plugin.type` 与 `ChannelGroup.type`
  是两个概念，见 [CONTEXT.md](../CONTEXT.md)。
- 后端入口 `backend/<amd|arm>/` 与前端入口 `frontend/entry.js` 都由目录约定声明，不进入清单；
  两者都必须提供，但可以是空实现/no-op。
- 平台名固定为 `amd`/`arm`。host 按当前平台查对应目录，缺失即报错（安装被拒）。

## ABI

v1 不做任何 ABI 兼容性校验。清单不含 `core_abi`，host 不比对版本标记；插件与当前
Revlm/httplib ABI 一起冷启动升级，错配由用户负责，可能崩溃。这是"安装即完全信任"前提下
有意接受的耦合。

## 生命周期符号

生命周期符号使用 `extern "C" void` ABI：正常返回表示成功；抛出 C++ 异常表示失败，由 host 捕获并按生命周期规则处理。无参数生命周期函数需要访问数据库时，插件直接调用核心公开的全局数据库访问符号，复用当前 Revlm C++ ABI。

- `revlm_plugin_migrate()`：每次启用插件冷启动时由 host 调用；禁用插件跳过迁移，重新启用后等下一次冷启动执行。插件内部执行迁移 SQL 并保证幂等。安装/更新只落盘，等外部冷启动时执行迁移。核心 schema 先执行，插件迁移在其后。符号缺失时按 no-op 处理。一次插件迁移在单个数据库事务中执行；失败自动回滚本次迁移。
- `revlm_plugin_cleanup()`：卸载请求只标记 pending；下一次 bootstrap 在启动 worker 前加载待卸载插件并调用 cleanup。符号缺失时按 no-op 处理。清理失败时 host 保留包、标记 `failed`、不自动重试；用户再次显式发起卸载时重新调用 cleanup，成功后删除包，失败则继续保留并标记 `failed`。

插件数据库的 schema、迁移、业务数据与卸载清理全部归插件；核心不解析插件的数据库内容。

## 加载顺序

已启用插件按插件 `id` 字典序进入 `LD_PRELOAD`。多个插件定义同一符号时，字典序靠前的模块
获胜；需要协作时作者自行用 `RTLD_NEXT` 链式调用。host 不校验同名符号冲突。

## 前端

`frontend/entry.js` 是普通 ESM，可为空。核心前端只动态 `import` 该模块，入口依靠模块副作用自行挂载；host 不读取 export、不传入宿主上下文，也不提供前端 SDK。`frontend/` 下的相对资源走同一路径前缀。资产列表固定为 worker 启动时的包快照，上传或停用不会在运行中改变前端。
