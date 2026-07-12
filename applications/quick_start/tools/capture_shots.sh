#!/usr/bin/env bash
#
# 生成教程页配图: 用 drm_app_neo 的 PC 模拟器逐屏截图 (仅 360 档, 720p 机种
# 显示时拉伸), 外加从演示视频抽一帧当"播放态"配图。产物为 JPEG q70
# (LVGL 内置 TJPGD 解码, 比 PNG 省一大截)。产物提交进仓库, 本脚本仅供复现。
#
# 依赖: sim 可构建 (SDL2/freetype/libpng)、ffmpeg、python3-PIL。
# 用法: tools/capture_shots.sh [演示视频.mp4]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(dirname "$HERE")"                       # applications/quick_start
APPS_DIR="$(dirname "$APP_DIR")"                   # applications/
SIM_DIR="$(cd "$APPS_DIR/../../drm_app_neo/sim" && pwd)"
VIDEO="${1:-$HOME/Videos/epass_next_test_fia.mp4}"
OUT="$APP_DIR/assets/shots"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

# --- 播放态帧 (720x1280 源 → 360x640) ---
ffmpeg -loglevel error -y -ss 8 -i "$VIDEO" -frames:v 1 -vf scale=360:640 "$TMP/playback.png"

# --- 构建模拟器 ---
cmake -S "$SIM_DIR" -B "$SIM_DIR/build" >/dev/null
cmake --build "$SIM_DIR/build" -j"$(nproc)" >/dev/null
BIN="$SIM_DIR/build/epass_ui_sim_360x640"

# 幕帘上方露出处铺播放态帧, 模拟真机 UI 层下的立绘视频层
BG="$TMP/bg.bmp"
ffmpeg -loglevel error -y -i "$TMP/playback.png" "$BG"

# --- 逐屏截图 ---
# screen_id_t: 0=mainmenu 1=oplist 2=sysinfo 6=settings 7=applist 10=usbselect
# (displayimg 不截: 底部路径标签会泄露宿主机路径, 教程页也用不上)
declare -A SCREENS=( [mainmenu]=0 [oplist]=1 [sysinfo]=2 [settings]=6 [applist]=7 [usbselect]=10 )
for name in "${!SCREENS[@]}"; do
    SIM_SCREEN="${SCREENS[$name]}" SIM_SHOT="$TMP/$name.bmp" SIM_BG="$BG" \
        SIM_APPS_DIR="$APPS_DIR" \
        "$BIN" >/dev/null 2>&1
done

# --- 转 JPEG q70 (baseline, TJPGD 可解), 预缩放到两档显示尺寸 ---
# TJPGD 分带解码, LVGL 画不了带变换的 JPEG → 按显示原尺寸出图, 设备上 1:1 贴,
# 零缩放开销。180x320 给 360p 机种, 360x640 给 720p 机种 (页面内均为半屏宽)。
python3 - "$TMP" "$OUT" <<'EOF'
import os, sys
from PIL import Image
tmp, out = sys.argv[1], sys.argv[2]
for w, h in ((180, 320), (360, 640)):
    os.makedirs(os.path.join(out, str(w)), exist_ok=True)
    for f in sorted(os.listdir(tmp)):
        name, ext = os.path.splitext(f)
        if ext not in (".bmp", ".png") or name == "bg":
            continue
        dst = os.path.join(out, str(w), name + ".jpg")
        img = Image.open(os.path.join(tmp, f)).convert("RGB")
        img.resize((w, h), Image.LANCZOS).save(dst, quality=70, optimize=True)
        print(f"  {dst} ({os.path.getsize(dst)//1024}K)")
EOF

echo "done."
