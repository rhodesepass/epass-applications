# epass devbox

一个容器 = Claude Code + emcc + buildroot 交叉工具链。进去对 agent 提需求,
它生成/修改应用,编出 **浏览器 wasm 预览** 和 **设备安装包**,都从 :8080 拿。

## 一次性准备

```sh
# 1) buildroot SDK tarball(自带 relocate-sdk.sh, 130MB)
cd ../buildroot && make sdk
cp output/images/arm-buildroot-linux-musleabi_sdk-buildroot.tar.gz \
   ../epass_applications/cloud/data/sdk.tar.gz

# 2) 镜像(fonts 仓库私有时加 --secret id=git_token,src=<token文件>)
docker build -t epass-devbox -f cloud/devbox/Dockerfile cloud/
```

## 日常使用

```sh
export ANTHROPIC_BASE_URL=...          # 企业转发
export ANTHROPIC_API_KEY=...
export ANTHROPIC_MODEL=...             # 默认模型名(转发侧要求显式指定时)
export ANTHROPIC_SMALL_FAST_MODEL=...  # 可选: 后台小任务模型
cloud/devbox/run.sh [会话名]            # 缺省时间戳; 同名则复用 workspace
```

进去就是 Claude Code,workspace 是本仓库的私有克隆(`cloud/data/workspaces/<会话名>/repo`,
agent 怎么折腾都不碰你的开发目录)。浏览器开 http://localhost:8080:

- 应用列表 = wasm 预览(`build-wasm/index.html`,构建脚本自动刷新)
- `packages/` = 设备安装包,下载后解到设备 `/app/` 即装

示例提示词:

> 做一个打砖块游戏,给我 wasm 预览和设备安装包

agent 侧的约定(模板、命令、双端约束)在仓库根 `CLAUDE.md`,随 workspace 克隆自动生效。

## 这轮刻意不做

- web 三面板(web terminal + code-server + preview)——下一轮 web 版
- 多会话编排 / 配额 / token 记账
- 容器出网白名单(本机自用,全放行;对外服务前必须收紧)
- 字体子集化(LVGL 应用 wasm data 仍 41MB)
- SDK 分发(镜像构建依赖本机 buildroot `make sdk` 产物)

## 已知边界

- 基础镜像是 debian:trixie 而非官方 emsdk:buildroot SDK 的交叉 gcc 在宿主机
  (新 glibc)上编出,emsdk 镜像的老 glibc 跑不动;debian 的 apt emscripten
  3.1.69 与本机开发环境完全一致
- 交叉编译走 `share/buildroot/toolchainfile.cmake`(自定位),SDK sysroot 自带
  libdrm/lvgl/epass-fonts 的 pkg-config,容器内交叉编译完全离线;宿主机与
  容器共用 `build-arm/` 时 package_app.sh 检测到缓存来自别的环境会自动重配
- emcc ports 缓存在镜像构建期预热(`/opt/epass/emcache`),运行期只读:
  新应用用到未预热的 flag 组合会当场报错——把组合补进 Dockerfile 预热段重建
- `run.sh` 的 workspace 来自 `git clone --local`,**未提交的改动不会进会话**
- video_player 的 vdec 栈(内核 uapi 头冲突)在容器里同样编不了 host 版,
  但交叉编译正常
