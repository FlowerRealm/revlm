# Kubernetes 集群部署 (完整版)

本页是唯一维护的 Kubernetes 部署路径: Helm values + 明确的前置资源。
集群、ingress-nginx、MySQL 自己先准备好, 不在仓库里维护通用安装器。

默认拓扑只跑 api。前端从 `frontend/` 构建为普通静态文件，部署到自有静态服务器或任意静态托管。Helm chart 不承载前端，因为默认 `flowerrealm/revlm` 镜像不包含 `frontend/dist`。

## 1. 准备前置资源

### MySQL

任何 8.x 兼容实例。需要的 schema 由应用启动时自动迁移; 你只需准备好库 + 账号:

```sql
CREATE DATABASE revlm CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'revlm'@'%' IDENTIFIED BY '...';
GRANT ALL ON revlm.* TO 'revlm'@'%';
```

DSN 形态:

```
revlm:PASSWORD@tcp(mysql.default.svc.cluster.local:3306)/revlm?parseTime=true&charset=utf8mb4&collation=utf8mb4_unicode_ci&time_zone=%27%2B00%3A00%27
```

### Redis

任何 Redis 7.x 兼容实例。用于跨 Pod 并发限制和缓存失效协调。未配置时降级为 Pod 内本地模式（单 Pod 部署不影响）。

```bash
kubectl -n revlm create secret generic revlm-env \
  --from-literal=REVLM_REDIS_PASSWORD='...'
```

DSN 形态通过 values 配置:

```yaml
redis:
  addr: redis.default.svc.cluster.local:6379
```

### ingress-nginx

任何 ingress-nginx 安装即可。chart 默认下发的 annotation 假设这是 ingress-nginx (启用了 `upstream-hash-by`)。

## 2. 创建 namespace 与 Secret

```bash
kubectl create namespace revlm
kubectl -n revlm create secret generic revlm-env \
  --from-literal=REVLM_DB_DSN='...' \
  --from-literal=SESSION_SECRET="$(openssl rand -hex 32)" \
  --from-literal=REVLM_ADMIN_API_KEY="$(openssl rand -hex 32)" \
  --from-literal=REVLM_REDIS_PASSWORD='...'
```

## 3. 写 values

最小生产 values:

```yaml
# /tmp/revlm-values.yaml
image:
  tag: v0.29.1                    # 使用不可变版本 tag；release workflow 不发布 latest

ingress:
  enabled: true
  className: nginx
  hosts:
    - api.revlm.example.com
  routes:
    - path: /v1
      component: api
    - path: /v1beta
      component: api
    - path: /api
      component: api
    - path: /oauth
      component: api
    - path: /auth/callback
      pathType: Exact
      component: api
  tls:
    - secretName: revlm-tls
      hosts: [api.revlm.example.com]
  userRouting:
    enabled: true
    upstreamHashBy: "$cookie_revlm_session"   # 若有稳定的会话标识

redis:
  addr: redis.default.svc.cluster.local:6379

components:
  api:
    role: api
    replicas: 3
    autoscaling:
      enabled: true
      minReplicas: 3
      maxReplicas: 12

serviceMonitor:
  enabled: true

networkPolicy:
  enabled: true
  ingressFromNamespaces:
    - { name: ingress-nginx }     # 放行 ingress-nginx 进入

```

## 4. 部署

```bash
helm upgrade --install revlm ./charts/revlm \
  -n revlm --create-namespace \
  -f /tmp/revlm-values.yaml
```

等所有 Deployment ready:

```bash
kubectl -n revlm rollout status deploy/revlm-revlm-api
```

## 5. 升级

```bash
helm upgrade revlm ./charts/revlm -n revlm \
  -f /tmp/revlm-values.yaml \
  --set image.tag=v0.29.1
```

回滚:

```bash
helm history  revlm -n revlm
helm rollback revlm <REVISION> -n revlm
```

## 6. NetworkPolicy 行为

`networkPolicy.enabled=true` 时为每个组件渲染一份 NetworkPolicy:

- 默认放行: 同 release 内其他组件 (按 `app.kubernetes.io/instance` selector)
- 额外放行: `ingressFromNamespaces` (按 `kubernetes.io/metadata.name`) 与 `ingressFromPodLabels`
- 出方向无限制 (egress 不在 chart 控制范围)

调整时确保把 `ingress-nginx` 所在 namespace 加入 `ingressFromNamespaces`, 否则外部流量被拒。

## 7. Metrics

应用公开 `/metrics` 端点（Prometheus text format），指标包括：
- `revlm_v1_requests_in_flight` — 当前 `/v1/*` 请求并发数
- `revlm_sse_connections_active` — 活跃 SSE 连接数
- `revlm_usage_finalize_queue_depth` — 异步用量队列深度
- `revlm_auth_cache_hits_total` — 认证缓存命中/未命中

开启 ServiceMonitor：

```yaml
serviceMonitor:
  enabled: true
```

chart 同时注入 `prometheus.io/scrape: "true"` pod annotations 作为回退方案（适用于没有 Prometheus Operator 的集群）。

HPA 可基于 `revlm_v1_requests_in_flight` 自定义指标自动扩缩（需要集群中运行 prometheus-adapter 或 KEDA 做指标桥接）：

```yaml
autoscaling:
  customMetrics:
    - type: Pods
      pods:
        metric:
          name: revlm_v1_requests_in_flight
        target:
          type: AverageValue
          averageValue: "5"
```

> `/metrics` 仅在 api 组件暴露。Helm chart 不渲染 frontend 组件，前端入口由独立静态部署负责。

## 8. Worker 节点

- 确保所有 worker 节点处于 `Ready,SchedulingEnabled` 状态。cordoned 节点不会接收调度。
- `requiredDuringScheduling` podAntiAffinity 要求 replicas ≤ 可用 worker 节点数。单节点集群需在 values 中显式设置 `affinity: {}` 覆盖回空。
- 可通过 `kubectl get nodes` 检查节点状态；`kubectl uncordon <node>` 恢复调度。
