#!/bin/sh
# 宿主机启动器: 给 agent 一个私有的仓库工作副本, 起 devbox 容器。
# 用法: cloud/devbox/run.sh [会话名]   (缺省用时间戳; 已有同名 workspace 则复用)
# 环境: ANTHROPIC_API_KEY / ANTHROPIC_BASE_URL 透传进容器(企业转发)。
set -e
cd "$(dirname "$0")/../.."

SID="${1:-$(date +%Y%m%d-%H%M%S)}"
WS="cloud/data/workspaces/$SID"

if [ ! -d "$WS/repo" ]; then
    mkdir -p "$WS"
    git clone --local . "$WS/repo"
    echo "== 新 workspace: $WS/repo"
else
    echo "== 复用 workspace: $WS/repo"
fi

exec docker run -it --rm \
    --name "epass-devbox-$SID" \
    -p 8080:8080 \
    -e ANTHROPIC_API_KEY \
    -e ANTHROPIC_BASE_URL \
    -e ANTHROPIC_MODEL \
    -e ANTHROPIC_SMALL_FAST_MODEL \
    -v "$(pwd)/$WS/repo:/workspace" \
    epass-devbox
