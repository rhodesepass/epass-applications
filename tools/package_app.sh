#!/bin/sh
# 交叉编译一个应用并打成设备安装包(解到设备 /app/ 即装)。
# 用法: tools/package_app.sh <app 目录名>
# 依赖: buildroot SDK, 路径用 EPASS_SDK 覆盖(默认 ../buildroot/output/host)
set -e
cd "$(dirname "$0")/.."

APP="${1:?用法: package_app.sh <app 目录名>}"
SDK="${EPASS_SDK:-../buildroot/output/host}"
BUILD=build-arm
PKGDIR=build-wasm/packages

[ -d "applications/$APP" ] || { echo "没有 applications/$APP"; exit 1; }
TOOLCHAIN="$SDK/share/buildroot/toolchainfile.cmake"
[ -f "$TOOLCHAIN" ] || \
    { echo "缺 SDK $SDK: 设 EPASS_SDK 或先构建 buildroot"; exit 1; }

# CMake 缓存烘死绝对路径, 宿主机/容器共用一个仓库时互相打架 —— 源目录
# 对不上就整个重配(几秒的事, 别修缓存)
if [ -f "$BUILD/CMakeCache.txt" ] && \
   ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=$(pwd)\$" "$BUILD/CMakeCache.txt"; then
    echo "== $BUILD 是别的环境配的, 重配"
    rm -rf "$BUILD"
fi

# toolchainfile.cmake 自带定位(按自身路径算 HOST_DIR), 不必 source
# environment-setup —— 后者是给交互 shell 用的(bash-only + alias)。
# 配置失败多半是残留的坏缓存(比如半截失败的配置), 清掉重来一次。
cfg() {
    cmake -S . -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$(realpath "$TOOLCHAIN")" \
        -DBUILD_TESTING=OFF >/dev/null
}
cfg || { echo "== 配置失败, 清 $BUILD 重试"; rm -rf "$BUILD"; cfg; }
cmake --build "$BUILD" --target "$APP" -j"$(nproc)"

# 只装本应用: 应用子目录的 build 树各有 cmake_install.cmake, 按目录装
# 不会把没编译的其他应用拖进来
cmake --install "$BUILD/applications/$APP" --strip >/dev/null

# app_ver 进文件名, 方便设备端区分版本
VER=$(python3 -c "import json;print(json.load(open('applications/$APP/appconfig.json',encoding='utf-8')).get('app_ver',0))" 2>/dev/null || echo 0)
mkdir -p "$PKGDIR"
TAR="$PKGDIR/$APP-v$VER.tar.gz"
tar -C dist -czf "$TAR" "$APP"

echo "==> $TAR  (设备上解到 /app/: tar -xzf $(basename "$TAR") -C /app/)"
