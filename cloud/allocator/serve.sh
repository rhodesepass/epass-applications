#!/bin/sh
# 分配器启动器: venv 装依赖(首次) → 起 app.py。
# 环境: EPASS_LLM_KEY(真 API key) / EPASS_LLM_UPSTREAM(企业转发 URL) 必填。
set -e
cd "$(dirname "$0")/../.."

: "${EPASS_LLM_KEY:?需要 EPASS_LLM_KEY (真 API key, 只存在于宿主)}"
: "${EPASS_LLM_UPSTREAM:?需要 EPASS_LLM_UPSTREAM (企业转发 URL)}"

VENV=cloud/data/venv
if [ ! -x "$VENV/bin/python" ]; then
    if command -v uv >/dev/null; then
        uv venv -q "$VENV"
        uv pip install -q -p "$VENV/bin/python" -r cloud/allocator/requirements.txt
    else
        python3 -m venv "$VENV"   # 需要 python3-venv 包
        "$VENV/bin/pip" install -q -r cloud/allocator/requirements.txt
    fi
fi

exec "$VENV/bin/python" cloud/allocator/app.py
