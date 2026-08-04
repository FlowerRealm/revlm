# Channel 是纯上游端点：type 上移到 ChannelGroup

状态：accepted

Channel 不再是协议承载单元。它只描述一个上游地址与凭据，字段收敛为 `id`、`name`、`status`、
`priority`、`base_url`、`api_key`；`type`、`price_multiplier`、`config_json`、模型列表全部删除。
协议类型由 ChannelGroup 拥有：一个 ChannelGroup 只有一个 `type`，`type` 与 `price_multiplier`
属于 ChannelGroup，不属于 Channel。协议分发以 API key 解析到的 ChannelGroup 为边界，再沿该组
的 `type` 找到插件；Channel 本身不携带任何协议或插件语义。

Channel 与 ChannelGroup 仍保持多对多成员关系（`channel_group_members`），但核心不做组间
`type` 一致性强校验：Channel 没有 `type`，同一个上游被挂进两个不同 `type` 的组属于用户配置
错误，由用户负责，核心不为此增加校验机器。

## 迁移

`channels.type` 删除时直接改 schema，不写数据回填迁移。`channel_groups` 新增 `type` 列并置为
必填；现有 `channels.type`（`openai_compatible`/`anthropic`）不迁移到任何新列，需要一次性重建
或手工重配。这是 v1 早期阶段的取舍：没有需要保护的现网数据，用不回填换取 schema 的干净。

## 已否决的方案

- 保留 `channels.type` 并继续作为协议信号：与"type 是组属性、分发以组为单位"矛盾，会把协议
  解释责任留在 channel 上。
- 写回填迁移把 `channels.type` 搬到 `channel_groups.type`：当前阶段无现网数据，不值得为假设的
  用户写迁移与双轨。
- 在 Channel 上做多组 `type` 一致性强校验：Channel 无 `type`，该校验没有对象；协议错配是用户
  配置错误，核心不为"完全信任"前提增加防护。
