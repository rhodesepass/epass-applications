#!/bin/sh
# 把一个 epass_game 系游戏编成浏览器 wasm。
# 用法: tools/build_wasm_game.sh <app 目录名> [输出目录]
# 例:   tools/build_wasm_game.sh snake && python3 -m http.server -d build-wasm/snake
set -e
cd "$(dirname "$0")/.."

APP="${1:?用法: build_wasm_game.sh <app 目录名> [输出目录]}"
OUT="${2:-build-wasm/$APP}"
GAME=applications/common/epass_game

# 源码可能在 src/ 下(snake/tetris/2048), 也可能直接在 app 根(flappy/epniccc)
SRC="applications/$APP/src"
[ -d "$SRC" ] || SRC="applications/$APP"

# 数据文件(如 epniccc 的 scene1.bin)嵌进 wasm 虚拟文件系统, 运行时路径不变
EXTRA=""
for f in applications/"$APP"/*.bin; do
    [ -e "$f" ] && EXTRA="$EXTRA --embed-file $f@$(basename "$f")"
done

mkdir -p "$OUT"
emcc \
    "$SRC"/*.c \
    $EXTRA \
    "$GAME"/src/game_draw.c \
    "$GAME"/src/game_platform_wasm.c \
    -I"$GAME"/include \
    -O2 -Wall -Wextra \
    -sALLOW_MEMORY_GROWTH=1 \
    -sMINIFY_HTML=0 \
    --shell-file tools/wasm_shell.html \
    -o "$OUT"/index.html

echo "==> $OUT/index.html"

# 编完顺手刷新预览首页, 免得索引落后于实际产物
if [ -x tools/build_wasm_index.sh ]; then
    tools/build_wasm_index.sh
fi
