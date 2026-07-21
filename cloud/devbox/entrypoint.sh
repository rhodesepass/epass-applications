#!/bin/bash
# devbox 容器入口: 起预览静态服务(后台)→ 进 Claude Code(或指定命令)。
# /workspace = 挂载进来的仓库工作副本。
set -e
cd /workspace

# 预览首页可能还没生成(全新 workspace), 先建目录保证 server 能起
mkdir -p build-wasm/packages
[ -f build-wasm/index.html ] || [ ! -x tools/build_wasm_index.sh ] || \
    tools/build_wasm_index.sh || true

python3 -m http.server 8080 -d build-wasm >/tmp/http.log 2>&1 &

echo "== epass devbox =="
echo "预览/下载: http://localhost:8080  (wasm 应用列表 + packages/ 设备安装包)"
echo

if [ $# -gt 0 ]; then
    exec "$@"
fi
exec claude
