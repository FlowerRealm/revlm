<div align="center">

# Revlm

**企业级自托管 AI API 平台 — 统一接入、精细计费、可控运营**

[产品能力](#产品能力) · [适用场景](#适用场景) · [部署](#部署) · [文档](#文档)

</div>

---

## 简介

Revlm 是面向 **API 转售商、AI 服务商与企业 IT 团队** 的自托管 AI 网关平台。将多家上游模型供应商聚合为统一的 OpenAI 兼容接口，在同一套控制台里完成用户管理、渠道调度、用量核算与余额计费——数据与密钥留在你自己的基础设施上。

无需把客户流量和用户数据交给第三方 SaaS；Revlm 让你以自有品牌、自有域名、自有定价策略运营 AI API 服务。

## 适用场景

| 场景 | Revlm 能做什么 |
|------|----------------|
| **API 转售 / 代理商** | 对接多个上游渠道，按渠道组灵活调度，对客户按量计费 |
| **企业内部 AI 平台** | 为员工或业务线发放 API Token，统一管控用量与成本 |
| **私有化部署** | 全栈自托管，会话、用量、余额数据存储于自有 MySQL |
| **多实例生产运维** | Docker / Kubernetes 部署，健康探针与 Prometheus 指标开箱即用 |

## 产品能力

**统一 API 出口**
- OpenAI 兼容数据面：`/v1/chat/completions`、`/v1/messages`、`/v1/responses` 等
- 流式响应支持，请求级用量自动记录

**渠道与调度**
- 多上游渠道集中管理，支持渠道组分组与优先级调度
- 自动 failover、并发控制与上游重试

**用户与鉴权**
- 多用户体系，每用户可创建多个 API Token
- Token 绑定渠道组与模型别名，精细控制可访问范围

**计费与运营**
- 按量扣费（PAYGO），用户余额实时查询
- 请求级用量事件、时序统计与管理端 Dashboard
- 管理员充值入账、用户与渠道全生命周期管理

**Web 控制台**
- 用户自助：Token 管理、用量查询、账户设置
- 管理后台：渠道/渠道组、用户、系统设置、全局用量审计

## 工作原理

```
  你的客户 / 业务系统
         │
         ▼
  ┌──────────────┐      ┌─────────────────────────┐
  │  Web 控制台   │      │      Revlm 网关          │
  │  (品牌门户)   │ ───► │  鉴权 · 调度 · 计费 · 审计  │
  └──────────────┘      └───────────┬─────────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
               上游渠道 A      上游渠道 B       上游渠道 C
              (OpenAI 等)    (Anthropic 等)    (其他供应商)
```

控制台与 API 网关可同域部署，也可拆分：静态门户独立托管，API 路径反代至网关服务。详见 [部署文档](docs/deployment/overview.md)。

## 部署

Revlm 提供两种交付物：

- **API 网关**：Docker 镜像或 Helm chart，承载鉴权、调度、计费与数据面
- **Web 控制台**：独立构建的静态站点，按你的品牌域名托管

### Docker（推荐起步）

```bash
docker build -t revlm .
docker run -d --name revlm -p 8080:8080 \
  -e REVLM_DB_DSN='user:pass@tcp(db-host:3306)/revlm?parseTime=true&charset=utf8mb4' \
  -e SESSION_SECRET='<强随机密钥>' \
  revlm
```

### Kubernetes

使用 [`charts/revlm`](charts/revlm/) Helm chart 部署 API 网关，支持多副本、HPA、Ingress 路由拆分。前端控制台需另行托管。

完整部署指南：[docs/deployment/overview.md](docs/deployment/overview.md)

## 文档

| 文档 | 内容 |
|------|------|
| [部署总览](docs/deployment/overview.md) | Docker、Helm、路由与域名配置 |
| [API 手册](docs/reference/api.md) | 控制面与数据面接口 |
| [架构说明](docs/reference/architecture.md) | 系统组成与请求链路 |
| [数据模型](docs/reference/data-model.md) | 用户、渠道、用量、计费实体 |
| [安全](SECURITY.md) | 生产环境安全要求与漏洞报告 |

## 安全

生产部署须配置强随机的 `SESSION_SECRET`、独立的数据库凭据，并限制数据库与 Redis 的网络访问。详见 [SECURITY.md](SECURITY.md)。

## License

[MIT](LICENSE) © Revlm Contributors
