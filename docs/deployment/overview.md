# 部署总览

## 心智模型

Revlm 有两个独立部署物：

- **API 网关**：Docker/Helm 镜像，内含 `/revlm` 可执行文件（启动时 ODB `ensure_schema`）
- **Web 控制台**：`frontend/dist` 静态文件，由 nginx/Caddy 等独立托管

镜像不会构建或复制 `frontend/dist`。

## 路由边界

前端域名若要承载完整 Web 体验，需将以下路径反代至 API 网关：

- `/api`
- `/v1`
- `/v1beta`
- `/oauth`
- `/auth/callback`
- `/healthz`
- `/livez`
- `/readyz`

其余路径由静态站点返回 `frontend/dist`，未知非资源路径走 SPA fallback。

## 按场景选

- **Docker**：单机或小规模生产，见下方示例
- **Kubernetes**：`charts/revlm` Helm chart，多副本与 HPA
- **自有服务器**：nginx/Caddy 托管 `frontend/dist`，API 路径反代至网关容器或进程

## Docker

仓库根目录 `Dockerfile` 构建 C++ 二进制并打包进 distroless 镜像，入口为 `/revlm`。

```bash
docker build -t revlm .
docker run -d --name revlm -p 8080:8080 \
  -e REVLM_DB_DSN='user:pass@tcp(db-host:3306)/revlm?parseTime=true&charset=utf8mb4' \
  -e SESSION_SECRET='<强随机密钥>' \
  revlm
```

必填环境变量：`REVLM_DB_DSN`、`SESSION_SECRET`。

## Helm chart

`charts/revlm` 是 data-driven Helm chart：

- `.Values.components` 定义 API 组件，默认只有 `api`
- 每个组件独立得到 Deployment + Service + HPA + PDB + ServiceAccount
- Ingress 用 `routes` 把路径映射到组件名
- 前端必须独立部署，chart 不承载静态资源

最小 API ingress：

```yaml
components:
  api:
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
