# 部署总览

## 心智模型

Revlm 有两个独立部署物：

- **API 网关**：Docker/Helm 镜像，内含 `/revlm` bootstrap 与 `/revlm-worker`（bootstrap 初始化 schema、
  调用启用插件生命周期符号，然后以预加载模块 `exec` worker）
- **Web 控制台**：`frontend/dist` 静态文件，由 nginx/Caddy 等独立托管

镜像不会构建或复制 `frontend/dist`。

## 路由边界

前端域名若要承载完整 Web 体验，需将以下路径反代至 API 网关：

- `/api`
- `/v1`
- `/oauth`
- `/auth/callback`
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
  -v revlm-plugins:/var/lib/revlm/plugins \
  -e REVLM_DB_DSN='user:pass@tcp(db-host:3306)/revlm?parseTime=true&charset=utf8mb4' \
  revlm
```

必填环境变量：`REVLM_DB_DSN`。

## 数据面插件

`REVLM_PLUGIN_DIR`（默认 `/var/lib/revlm/plugins`）保存 root 上传的 `.revlm-plugin` 包。它必须跨容器重启保留；Docker 镜像已经把该目录声明为 volume，生产环境建议显式挂载具备 UID `65532` 写权限的卷。

镜像内兼容插件放在只读的 `REVLM_SYSTEM_PLUGIN_DIR`（默认 `/usr/share/revlm/plugins`）。多架构镜像会在各自的构建平台编译并内置匹配的 OpenAI、Anthropic 包；它们可由 root 停用但不可从镜像删除。上传同 ID 包会覆盖镜像包；卸载覆盖包并重启后会自动恢复镜像版本。所有安装、启用、停用、卸载均只标记状态：bootstrap 在下一次启动将模块置入 Linux `LD_PRELOAD` 后 `exec` worker，服务不会热加载模块。

插件不受 SDK 或能力白名单限制，能够覆盖任意可插桩的公开核心 C++ 符号。因此生产包必须与正在
运行的镜像使用同一版本的 Revlm headers、编译器、标准库和 `librevlm_core`；不要把不同发行版的
预编译 `.so` 混用。

Kubernetes 多副本必须将同一个 RWX PVC 挂到每一个 API Pod：

```yaml
components:
  api:
    extraVolumes:
      - name: revlm-plugins
        persistentVolumeClaim:
          claimName: revlm-plugins-rwx
    extraVolumeMounts:
      - name: revlm-plugins
        mountPath: /var/lib/revlm/plugins
```

如果使用 ingress-nginx，上传包的 body 上限也要高于包大小；应用默认接受最大 256 MiB，可通过 `REVLM_PLUGIN_MAX_ARCHIVE_BYTES` 调整。

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
    - { path: /api, pathType: Prefix, component: api }
    - { path: /oauth, pathType: Prefix, component: api }
    - { path: /auth/callback, pathType: Exact, component: api }
```

镜像版本通过 `charts/revlm/values.yaml` 的 `image.tag` 控制，`charts/revlm/Chart.yaml` 的 `appVersion` 必须与它保持一致。

完整字段见 `charts/revlm/values.yaml`，校验规则见 `charts/revlm/values.schema.json`。
