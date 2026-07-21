#!/bin/sh
# 把一个 LVGL 系应用编成浏览器 wasm(hal_display/hal_input/hal_run 的 wasm 后端)。
# 用法: tools/build_wasm_lvgl.sh quick_start [输出目录]
# 依赖: build/_lvgl/lvgl (原生 cmake 配置时 FetchContent 拉下来的 v9.5.0 源码)
set -e
cd "$(dirname "$0")/.."

# Debian 包的 /usr/share/emscripten/cache 只读且 FROZEN_CACHE=True, 而
# ports(freetype/libpng/zlib) 需要往缓存里下载+编译。挪到用户目录并解冻;
# 先把系统预编的 sysroot 种进去, 免得全量重建。
# (官方 emsdk 环境如 devbox 镜像没有这个目录, 缓存已预热, 整块跳过)
if [ -d /usr/share/emscripten/cache ]; then
    export EM_CACHE="${EM_CACHE:-$HOME/.cache/emscripten}"
    export EM_FROZEN_CACHE=0
    if [ ! -e "$EM_CACHE/sysroot_install.stamp" ]; then
        mkdir -p "$EM_CACHE"
        cp -r /usr/share/emscripten/cache/. "$EM_CACHE"/
        chmod -R u+w "$EM_CACHE"
    fi
fi

APP="${1:?用法: build_wasm_lvgl.sh <app 目录名> [输出目录]}"
OUT="${2:-build-wasm/$APP}"
# 路径可被 env 覆盖(devbox 镜像里 ENV 预设), 不设则走本机默认。
# EPASS_LVGL_DIR 末级目录名必须是 lvgl: -I 用它的父目录解析 <lvgl/lvgl.h>。
LVGL="${EPASS_LVGL_DIR:-build/_lvgl/lvgl}"
FONTS="${EPASS_FONTS_DIR:-../epass-fonts/original}"

[ -d "$LVGL" ] || { echo "缺 $LVGL: 先跑一次原生 cmake 配置, 或设 EPASS_LVGL_DIR"; exit 1; }
[ -f "$FONTS/SourceHanSerifSC-Heavy.otf" ] || \
    { echo "缺字体 $FONTS: clone ../epass-fonts 或设 EPASS_FONTS_DIR"; exit 1; }

# app 源码: CMake 里 add_executable + *_core 的并集, 按目录约定收。
# wasm_exclude.txt 列出的文件(碰内核接口的探测代码)换成 *_wasm.c 桩。
APP_SRCS=$(find "applications/$APP/src" -name '*.c')
if [ -f "applications/$APP/wasm_exclude.txt" ]; then
    while read -r line; do
        [ -z "$line" ] && continue
        APP_SRCS=$(printf '%s\n' "$APP_SRCS" |
            grep -v "applications/$APP/$line\$" || true)
    done < "applications/$APP/wasm_exclude.txt"
fi

# 数据文件: 字体按角色兜底名放 /fonts(load_font 的 EPASS_FONTS_DIR 覆盖),
# assets 原样放 /assets(base_dir 回退 cwd=/)。优先源码树(改了即时生效,
# 且容器里没有 dist/), dist 仅作兜底。
EMBED=""
if [ -d "applications/$APP/assets" ]; then
    EMBED="--preload-file applications/$APP/assets@/assets"
elif [ -d "dist/$APP/assets" ]; then
    EMBED="--preload-file dist/$APP/assets@/assets"
fi

# fg_ext 应用(appconfig.json 的 type)按扩展名接文件启动: 换 VFS 外壳页——
# 不自启 main, 由页面把文件塞进 /data 后 callMain([路径]) 模拟文件管理器打开。
SHELL_FILE=tools/wasm_shell.html
PRE_JS=tools/wasm_lvgl_pre.js
VFS_FLAGS=""
if grep -q '"type"[[:space:]]*:[[:space:]]*"fg_ext"' "applications/$APP/appconfig.json" 2>/dev/null; then
    SHELL_FILE=tools/wasm_vfs_shell.html
    PRE_JS=tools/wasm_vfs_pre.js
    # /data 挂 IDBFS(上传与阅读进度跨刷新持久), 需要 idbfs 库
    VFS_FLAGS="-sEXPORTED_RUNTIME_METHODS=FS,callMain -lidbfs.js"
    # 示例文件放 /samples, 首次由外壳页播种进 /data(IDBFS 挂载会盖住挂载点)
    [ -d "tools/wasm_samples/$APP" ] && \
        EMBED="$EMBED --preload-file tools/wasm_samples/$APP@/samples"
fi

mkdir -p "$OUT"

# 设备的 lv_conf 开了 LV_OS_PTHREAD(独立 draw 线程)。wasm 没编 pthread,
# LVGL 会在 lv_thread_sync_wait 里等一个永远不跑的渲染线程(实测浏览器整页
# 冻死)。生成同步渲染的变体, 其余配置与设备一致。
LV_CONF_WASM="$OUT/.lv_conf_wasm.h"
sed 's/#define LV_USE_OS   LV_OS_PTHREAD/#define LV_USE_OS   LV_OS_NONE/' \
    lv_conf.h > "$LV_CONF_WASM"

# ASYNCIFY: 让 emscripten_sleep 真挂起 wasm 并让出主线程, hal_run/hal_idle
# 的 wasm 后端因此能用与 epass 相同的阻塞式 while(含平台层逐步改 alpha 的
# 阻塞式过渡)。代价是 wasm 体积变大, 预览场景可接受。
emcc \
    $APP_SRCS \
    hal/src/hal_display_wasm.c \
    hal/src/hal_input_wasm.c \
    hal/src/hal_run_wasm.c \
    hal/src/log.c \
    $(find "$LVGL/src" -name '*.c' -not -path '*/drivers/*') \
    -I hal/include -I hal/src -I hal/wasm_compat \
    -I "$LVGL" -I "$(dirname "$LVGL")" \
    -I "applications/$APP/src" \
    -I "applications/$APP/src/port" \
    -I "applications/$APP/src/ui" \
    -I "applications/$APP/src/system" \
    -DLV_CONF_PATH="\"$(realpath "$LV_CONF_WASM")\"" \
    -DFONT_REGISTRY_DIR="\"/fonts\"" \
    -O2 -Wall \
    -sUSE_FREETYPE=1 \
    -sUSE_LIBPNG=1 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sMINIFY_HTML=0 \
    -sASYNCIFY \
    $VFS_FLAGS \
    --preload-file "$FONTS/SourceHanSerifSC-Heavy.otf@/fonts/SourceHanSerifSC-Heavy.otf" \
    --preload-file "$FONTS/SourceHanSansSC-Regular.otf@/fonts/SourceHanSansSC-Regular.otf" \
    $EMBED \
    --pre-js "$PRE_JS" \
    --shell-file "$SHELL_FILE" \
    -o "$OUT"/index.html

echo "==> $OUT/index.html"

# 编完顺手刷新预览首页
if [ -x tools/build_wasm_index.sh ]; then
    tools/build_wasm_index.sh
fi
