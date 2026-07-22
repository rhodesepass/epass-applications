#!/bin/sh
# 一键掐死: 分配器进程 + 所有 epass 容器(epass-web-* 试用 / epass-devbox-* CLI)。
# 直接 rm -f, 不走归档; workspace 留在原地, 下次起分配器时会归档再清。
# 用法: cloud/killall.sh          只杀 epass 相关
#       cloud/killall.sh --all    宿主上所有容器一个不留
set -e

pkill -f 'cloud/allocator/app\.py' 2>/dev/null && echo "== 分配器已停" || true

if [ "$1" = "--all" ]; then
    ids=$(docker ps -aq)
else
    ids=$(docker ps -aq --filter name=epass-web- --filter name=epass-devbox-)
fi

if [ -n "$ids" ]; then
    docker rm -f $ids
    echo "== 已掐死 $(echo "$ids" | wc -l) 个容器"
else
    echo "== 没有容器在跑"
fi
