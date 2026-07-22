# epass devbox: Claude Code + emcc + buildroot 交叉工具链, 一个容器全包。
# 构建上下文是 cloud/(别用仓库根, 免得把几十 MB 产物送进 daemon):
#   docker build -t epass-devbox -f cloud/devbox/Dockerfile cloud/
# 前置: buildroot `make sdk` 的 tarball(自带 relocate-sdk.sh)放
#   cloud/data/sdk.tar.gz
# 私有 fonts 仓库时: --secret id=git_token,src=<token文件>
#
# 基础镜像用 debian 而非官方 emsdk: buildroot SDK 的交叉 gcc 在宿主机
# (新 glibc)上编出, emsdk 镜像的老 glibc 跑不动它; debian trixie 与宿主
# 同代, 且 apt 版 emscripten 3.1.69 与本机开发环境完全一致。
FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        emscripten cmake make git jq file ca-certificates curl \
        python3 python3-pil nodejs npm \
    && rm -rf /var/lib/apt/lists/*

# ---- buildroot SDK(交叉编译 F1C 设备二进制) ----
COPY data/sdk.tar.gz /tmp/sdk.tar.gz
RUN mkdir -p /opt/epass/sdk && \
    tar -xzf /tmp/sdk.tar.gz --strip-components=1 -C /opt/epass/sdk && \
    /opt/epass/sdk/relocate-sdk.sh && rm /tmp/sdk.tar.gz

# ---- LVGL 源码(与根 CMakeLists 的 FetchContent 同源同版本) ----
# 目录必须叫 .../lvgl: 构建脚本用其父目录 -I 解析 <lvgl/lvgl.h>
RUN mkdir -p /opt/epass/lvgl/lvgl && \
    curl -fsSL https://github.com/lvgl/lvgl/archive/refs/tags/v9.5.0.tar.gz \
    | tar -xz --strip-components=1 -C /opt/epass/lvgl/lvgl

# ---- 字体仓库(wasm 构建 --preload-file 用) ----
RUN --mount=type=secret,id=git_token,required=false \
    TOKEN=""; [ -s /run/secrets/git_token ] && TOKEN="x-access-token:$(cat /run/secrets/git_token)@"; \
    git clone --depth 1 "https://${TOKEN}github.com/rhodesepass/epass-fonts.git" \
        /opt/epass/epass-fonts

# ---- Claude Code CLI ----
RUN npm install -g @anthropic-ai/claude-code

# ---- emcc 缓存: Debian 系统缓存只读且冻结, 种到 /opt 后按两条构建路径
#      的 flag 组合各预热一次(freetype/libpng/zlib ports 需要联网下载,
#      只在镜像构建期发生; 运行期缓存只读, 漏预热会当场报错) ----
ENV EM_CACHE=/opt/epass/emcache
RUN mkdir -p "$EM_CACHE" && \
    cp -r /usr/share/emscripten/cache/. "$EM_CACHE"/ && \
    chmod -R u+w "$EM_CACHE"
COPY devbox/prewarm.c /tmp/prewarm.c
RUN cd /tmp && export EM_FROZEN_CACHE=0 && \
    emcc prewarm.c -O2 -sALLOW_MEMORY_GROWTH=1 -o a.js && \
    emcc prewarm.c -O2 -sALLOW_MEMORY_GROWTH=1 -sASYNCIFY \
        -sUSE_FREETYPE=1 -sUSE_LIBPNG=1 -o b.js && \
    rm -f prewarm.c a.* b.*

# host 侧构建+ctest 用的工具链(与开发机同构): agent 的"改完先跑单测"
# 循环靠它, 缺了会退化成瞎改
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev libdrm-dev libpng-dev libfreetype-dev pkgconf \
    && rm -rf /var/lib/apt/lists/*

# 缓存要保持可写: emcc 运行期会写 symbol_lists 及其锁文件(容器一次性,
# 弄脏了无所谓); 其余 /opt/epass 只读
RUN chmod -R a+rX /opt/epass && chmod -R a+rwX /opt/epass/emcache && \
    useradd -m -u 1000 -o -s /bin/bash dev

# claude 只跟企业转发说话: 掐掉更新检查/遥测/错误上报这些对
# api.anthropic.com 的旁路访问(出网受限环境下它们只会报错刷屏)
ENV DISABLE_AUTOUPDATER=1 \
    DISABLE_ERROR_REPORTING=1 \
    DISABLE_TELEMETRY=1 \
    CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1

# ---- ttyd(web 模式的浏览器终端, claude 跑在这里; trixie 无此包,
#      用官方 release 的静态 musl 二进制) ----
ARG TTYD_VERSION=1.7.7
RUN curl -fSL --retry 5 --retry-all-errors --retry-delay 3 \
        -o /usr/local/bin/ttyd \
        "https://github.com/tsl0922/ttyd/releases/download/${TTYD_VERSION}/ttyd.x86_64" && \
    chmod +x /usr/local/bin/ttyd

# ---- code-server(web 模式的浏览器版 VS Code; standalone tarball 自带
#      node, 不依赖 apt nodejs, 版本锁死可复现)。
#      放在最后: 别打掉前面昂贵的 emcc 预热层缓存 ----
ARG CODE_SERVER_VERSION=4.101.2
RUN curl -fSL --retry 5 --retry-all-errors --retry-delay 3 -C - \
        -o /tmp/code-server.tgz \
        "https://github.com/coder/code-server/releases/download/v${CODE_SERVER_VERSION}/code-server-${CODE_SERVER_VERSION}-linux-amd64.tar.gz" && \
    tar -xzf /tmp/code-server.tgz -C /opt && rm /tmp/code-server.tgz && \
    ln -s "/opt/code-server-${CODE_SERVER_VERSION}-linux-amd64/bin/code-server" \
        /usr/local/bin/code-server

ENV EPASS_SDK=/opt/epass/sdk \
    EPASS_LVGL_DIR=/opt/epass/lvgl/lvgl \
    EPASS_FONTS_DIR=/opt/epass/epass-fonts/original

COPY devbox/entrypoint.sh devbox/entrypoint-web.sh devbox/llm_forward.py \
    /usr/local/bin/
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/entrypoint-web.sh

USER dev
WORKDIR /workspace
EXPOSE 8080 8443 7681
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
