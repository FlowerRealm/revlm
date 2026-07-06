# 部署总览

## 心智模型

Revlm 现在有两个独立部署物：

- `api`：C++ 后端二进制，负责健康检查、API、管理面和支付回调。
- `frontend`：Vite/React 静态文件，构建产物是 `frontend/dist`。

发布的 Docker/Helm 镜像只覆盖 `api`，其中包含 `/revlm` 可执行文件和 `internal/store/migrations`。镜像不会构建或复制 `frontend/dist`；前端需要独立托管。

## 路由边界

前端域名如果要承载完整 Web 体验，需要把这些路径交给后端：

- `/api`
- `/v1`
- `/v1beta`
- `/oauth`
- `/auth/callback`
- `/healthz`
- `/livez`
- `/readyz`

其余路径由静态站点直接返回 `frontend/dist` 内容，未知非资源路径走 SPA fallback。

## 按场景选

- 自有服务器：nginx / Caddy 托管 `frontend/dist`，并把上述后端路径反代到 C++ API 服务。
- Kubernetes：Helm chart 只部署 API；前端仍作为独立静态应用部署。

## Docker

仓库根目录 `Dockerfile` 直接用 `g++` 构建 C++ 二进制，然后把它和 MariaDB client runtime、SQL migrations 拷进 distroless 镜像。容器入口固定是：

```text
/revlm
```

运行时角色由环境变量决定：

- `REVLM_NODE_ROLE=api`：只提供后端 API。
- `REVLM_NODE_ROLE=web`：只提供静态文件和反代。
- `REVLM_NODE_ROLE=all`：同进程同时提供 API 和静态入口。

`api` / `all` role 需要 `REVLM_DB_DSN` 与 `SESSION_SECRET`；`web` role 需要 `REVLM_PROXY_UPSTREAM_BASE_URL`。

## Helm chart

`charts/revlm` 是只面向 API 镜像的 data-driven Helm chart：

- `.Values.components` 定义一组 API 组件，默认只有 `api`
- 每个组件独立得到 Deployment + Service + HPA + PDB + ServiceAccount
- Ingress 用 `routes` 把路径映射到组件名
- chart 会拒绝 `role: web` 之类的前端组件；静态前端必须独立部署

最小 API ingress：

```yaml
components:
  api:
    role: api
    replicas: 2

env:
  REVLM_ENV: prod

secret:
  existingSecret: revlm-env

ingress:
  enabled: true
  hosts:
    - api.revlm.example.com
  routes:
    - { path: /v1, pathType: Prefix, component: api }
    - { path: /v1beta, pathType: Prefix, component: api }
    - { path: /api, pathType: Prefix, component: api }
    - { path: /oauth, pathType: Prefix, component: api }
    - { path: /auth/callback, pathType: Exact, component: api }
```

镜像版本通过 `charts/revlm/values.yaml` 的 `image.tag` 控制，`charts/revlm/Chart.yaml` 的 `appVersion` 必须与它保持一致。

完整字段见 `charts/revlm/values.yaml`，校验规则见 `charts/revlm/values.schema.json`。
