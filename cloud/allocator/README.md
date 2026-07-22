# epass web 分配器

外网试用入口:浏览器点"开始试用"→ 分到一个 devbox 容器,页面三面板:
ttyd 终端(claude 跑这里,免初始化直接进)+ code-server(纯编辑器)+
wasm 预览。token 在 URL 里,就是访问凭据。

## 前置

- `epass-devbox` 镜像已 build(见 `../devbox/README.md`,现在带 code-server)

## 启动

```sh
export EPASS_LLM_KEY=...        # 真 API key, 只存在于宿主, 容器拿不到
export EPASS_LLM_UPSTREAM=...   # 企业转发 URL
export ANTHROPIC_MODEL=...      # 可选, 透传进容器
cloud/allocator/serve.sh
```

首次会建 venv(`cloud/data/venv`)并初始化 hub 仓库(`cloud/data/hub.git`,
从本仓库 bare clone)。公网 :8000;LLM 注入代理听 unix socket
(`cloud/data/llmsock/llm.sock`,挂载进容器,容器内 llm_forward.py 把
`127.0.0.1:9100` 接过去)——不走 TCP,外网碰不到,也不依赖宿主防火墙
对 docker bridge 的放行。

## 语义

- **会话生命周期**:60 分钟无活动(浏览器请求或 LLM 调用都算活动)销毁
  容器;分配器重启即清场,残留容器先归档再删。
- **代码归档**:每个会话从 hub clone 出 `trial/<会话名>` 分支;销毁前
  自动兜底 commit + push 回 hub。收作品:
  `git fetch cloud/data/hub.git trial/<会话名> && git checkout FETCH_HEAD`
- **不复活**:容器销毁后原 URL 显示 gone 页,分支纯归档。
- **key 注入**:容器 `ANTHROPIC_BASE_URL` 指向
  `http://127.0.0.1:9100/llm/<token>`(容器内转发器 → 分配器 unix socket),
  代理换上真 key(`x-api-key` 头)流式转发到 `EPASS_LLM_UPSTREAM`。
  只注入,不记账。
- **claude 免初始化**:容器预写 `~/.claude.json`(跳过向导/信任
  /workspace/预批占位 key),镜像 ENV 掐掉更新检查、遥测、错误上报
  (出网受限环境下这些对 api.anthropic.com 的旁路访问只会报错)。

## 防打爆(环境变量可调)

- `EPASS_MAX_SESSIONS=8` 并发容器上限;60 秒内创建超 3 次直接拒
- 每容器 `--memory 3g --cpus 2 --pids-limit 512`
- `IDLE_TIMEOUT=3600` 闲置秒数;`PUBLIC_PORT=8000`

## 这轮刻意不做

- TLS / 域名:对外暴露 :8000 前建议前面套一层 caddy/nginx 终结 TLS
- 容器出网白名单(试用容器仍全放行)
- token 记账 / 配额
