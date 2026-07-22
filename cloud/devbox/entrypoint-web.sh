#!/bin/bash
# web 模式入口: 由分配器以 `docker run ... epass-devbox entrypoint-web.sh` 启动,
# 经 entrypoint.sh 的 exec "$@" 进入(预览服务已在后台)。
# 三个面板: ttyd(:7681, claude 跑这里) + code-server(:8443, 纯编辑器)
# + 预览(:8080, entrypoint.sh 已起)。
set -e

mkdir -p ~/.local/share/code-server/User

# claude 的 ANTHROPIC_BASE_URL 指向 127.0.0.1:9100, 由转发器接到分配器的
# unix socket(key 注入在宿主侧, 容器全程见不到真 key)
python3 /usr/local/bin/llm_forward.py >/tmp/llm_forward.log 2>&1 &

# 跳过 claude 首启向导(主题选择/登录引导/自定义 key 确认), 信任 /workspace:
# 出网受限环境下向导的连通性检查只会把用户卡在报错上。
# approved 里是 ANTHROPIC_API_KEY 占位值(分配器设的 "proxy", 取末 20 字符)
[ -f ~/.claude.json ] || cat > ~/.claude.json <<'EOF'
{
  "hasCompletedOnboarding": true,
  "theme": "dark",
  "customApiKeyResponses": { "approved": ["proxy"], "rejected": [] },
  "projects": {
    "/workspace": {
      "hasTrustDialogAccepted": true,
      "hasCompletedProjectOnboarding": true
    }
  }
}
EOF

# code-server 只当编辑器: 关工作区信任弹窗, 不自动跑任务
cat > ~/.local/share/code-server/User/settings.json <<'EOF'
{
  "security.workspace.trust.enabled": false,
  "workbench.startupEditor": "none"
}
EOF

# 浏览器终端: 每个连接一个 claude; 退出后留 shell 而不是死终端
ttyd -W -p 7681 bash -c 'cd /workspace && claude; exec bash' \
    >/tmp/ttyd.log 2>&1 &

# --auth none 安全: 端口都不发布到宿主, 唯一入口是分配器带 token 的反代路径
exec code-server --bind-addr 0.0.0.0:8443 --auth none \
    --disable-telemetry --disable-update-check /workspace
