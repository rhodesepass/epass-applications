# epass_applications

F1C200s 电子通行证(360×640 竖屏、4 物理键)的应用仓库。每个应用是独立进程,
**一套源码两个目标**:epass 真机(DRM/evdev,交叉编译)和浏览器 wasm(emcc)。

## 结构

- `hal/` — 双后端 HAL:`hal_display`(层/缓冲/alpha)、`hal_input`(4 键)、
  `hal_run`(主循环)。epass 实现在 `hal/src/*.c`,wasm 实现在 `hal/src/*_wasm.c`
- `applications/common/epass_game/` — 游戏框架(软件渲染,见 `epass_game.h`)
- `applications/<app>/` — 各应用;`tools/` — 构建脚本;产物出口 `build-wasm/`

## 常用命令

| 目的 | 命令 |
|---|---|
| wasm 预览(游戏) | `tools/build_wasm_game.sh <app>` |
| wasm 预览(LVGL) | `tools/build_wasm_lvgl.sh <app>` |
| 设备安装包 | `tools/package_app.sh <app>`(ARM 交叉编译 + tar,解到设备 `/app/` 即装) |
| 本机(host)构建+测试 | `cmake -S . -B build && cmake --build build -j && ctest --test-dir build` |
| 同上,devbox 容器内 | 加 `-DFETCHCONTENT_SOURCE_DIR_LVGL=$EPASS_LVGL_DIR` 用镜像内 LVGL 源码,免网络拉取 |

wasm 产物落在 `build-wasm/<app>/`,设备包落在 `build-wasm/packages/`。
devbox 容器里 :8080 静态服务托管整个 `build-wasm/`,编完刷新浏览器即可。

## 新应用怎么加

两类应用,模板都在仓库里,**抄现有的,别发明新结构**:

### 游戏类(推荐起步,参考 `applications/snake/`)

四件套 + 注册,全部照 snake 抄:

1. `applications/<app>/appconfig.json` — `uuid` 现生成
   (`python3 -c "import uuid;print(uuid.uuid4())"`),`type: "fg"`,
   `screens` 三档照抄,`name` 用中文
2. `applications/<app>/CMakeLists.txt` — add_executable + link `epass_game` +
   install 三段(snake 的改个名即可)
3. `applications/<app>/src/main.c` — **tick 回调 + `game_run(platform, tick, userdata)`**,
   静态 app 结构体,零 `#ifdef`(双端复用靠链接不同平台实现,不靠宏)。
   逻辑单独放 `src/logic.c` + 单测(`tests/`,参考 snake)
4. `applications/<app>/icon.png` — 128×128 RGBA(PIL 画个占位即可)
5. `applications/CMakeLists.txt` 加一行 `add_subdirectory(<app>)`

约束:
- 画面 360×640 逻辑坐标(`GAME_LOGICAL_WIDTH/HEIGHT`),绘制只用
  `game_draw_*`;输入只用 `game_key_pressed/down/repeated`,键枚举是
  `GAME_KEY_UP / GAME_KEY_DOWN / GAME_KEY_OK / GAME_KEY_BACK`
  (物理 4 键:1=上/左、2=下/右、3=确认、4=返回)
- 循环里不要 `usleep`,用 `game_platform_idle(platform, ms)`
  (wasm 上 sleep 是忙等,rAF 本身就是节拍)
- 源码不 include 任何平台头(drm/evdev/emscripten),只 include `epass_game.h`
- `game_framebuffer_t` 是**按值的描述符**:`game_framebuffer_t fb;` +
  `game_platform_acquire_frame(&platform, &fb)`,绘制传 `&fb`(别声明成指针)
- 编译警告不许放过:wasm 无内存保护,incompatible pointer 这类警告在浏览器里
  的表现是"白屏且无报错"

### LVGL 类(参考 `applications/quick_start/`)

- main 同样 tick 化:`hal_run(tick)` + `hal_idle`(见 `hal/include/hal_run.h`)
- 字体用角色宏(`FONT_BODY_PATH` 等,`epass_fonts` INTERFACE 目标透传),
  不写死文件名
- 碰内核/设备节点的探测代码:列进 `applications/<app>/wasm_exclude.txt`,
  写 `src/system/*_wasm.c` 桩(参考 system_maintenance 的 `system_wasm.c`)
- 显示只用 wasm 可实现子集:init/init_layer/allocate/free/mount(_alpha)/
  set_layer_alpha/disable/destroy/display_size;**别碰 enqueue/dequeue**
  (异步队列是 epass-only 的视频路径)
- 文件型应用(`type: "fg_ext"` + `extensions`)会自动获得 VFS 外壳
  (文件管理器语义),构建脚本按 appconfig.json 识别

## 验证习惯

- 改完先 host 构建 + ctest,再编 wasm 在浏览器实测(键盘映射:1/↑/←、2/↓/→、
  3/Enter、4/Esc;**先点击页面拿焦点**)
- wasm 与设备行为差异大的地方(sleep/线程/文件系统)优先怀疑平台层,
  游戏/应用逻辑层是共享代码
