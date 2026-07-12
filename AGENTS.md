# 电子通行证 · 第三方应用开发指南

本仓库用于存放系统附加程序。注册格式以启动器 `drm_app_neo` 的解析逻辑为准；图形接口以 `drm_app_neo/src/driver/drm_warpper.h` 为准（**纯 libdrm atomic**，不再使用 `srgn_drm` 自定义 ioctl）。

参考工程：

| 路径 | 用途 |
|------|------|
| `../c_example/` | 示例应用、`appconfig.json`、IPC 客户端、外设库 |
| `../drm_app_neo/` | 启动器实现、最新 DRM 封装 |
| `../c_example/.github/instructions/epass.instructions.md` | 历史开发说明（图形示例已过时，见下文） |

---

## 1. 应用注册：`appconfig.json`

每个应用一个目录，目录内必须有 `appconfig.json`（与可执行文件同级）。启动器扫描：

- NAND：`/app/<app_folder>/`
- SD（可选）：`/sd/app/<app_folder>/`

解析失败写入 `/root/apps.log`。`version` 必须为 `1`。

### 1.1 目录结构

```
/app/<app_folder>/
  appconfig.json
  <executable>          # executable.file 指向的二进制
  icon.png              # 可选
  fonts/ / assets/ ...  # 应用自带资源（相对路径即可）
```

启动器会先 `cd` 到应用目录再执行二进制，工作目录即应用包目录。

### 1.2 字段说明

| 字段 | 必选 | 类型 | 说明 |
|------|------|------|------|
| `version` | 是 | int | 必须为 `1` |
| `uuid` | 是 | string | `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`，用 `uuidgen` 生成 |
| `executable` | 是 | object / string | 推荐 `{"file":"二进制名"}`；也兼容顶层字符串 |
| `executable.file` | 是 | string | 相对应用目录的可执行文件名；启动器会 `chmod 0755` |
| `type` | 是 | string | `fg` / `bg` / `fg_ext` |
| `screens` | 是 | string[] | 支持的分辨率；须包含当前固件屏，否则拒绝加载 |
| `name` | 否 | string | 显示名；缺省用文件夹名 |
| `description` | 否 | string | 缺省 `"(无描述)"` |
| `icon` | 否 | string | 相对路径图标；缺/无效则用系统默认图标 |
| `extensions` | 否 | string[] | 文件关联后缀，如 `".txt"` |

无清单级 `permissions` 字段。硬件访问靠设备节点与库。

### 1.3 `type` 语义

| 值 | 行为 |
|----|------|
| `fg` | 前台。菜单可启动；启动器写 `/tmp/appstart` 并以 `EXITCODE_APPSTART` 退出，交出 DRM master |
| `bg` | 后台。`fork`+`execl`；菜单可启停；不占显示，通过 IPC 与主程序通信 |
| `fg_ext` | 仅文件关联启动；菜单直接点选会被拒绝 |

### 1.4 `screens` 取值

固件由 `cmake -DEPASS_SCREEN=...` 选定其一：

- `360x640`（F1C200s 默认）
- `480x854`
- `720x1280`

无特殊说明时，应用至少声明 `"360x640"`。

### 1.5 完整示例

前台最小模板：

```json
{
    "version": 1,
    "name": "程序模板",
    "uuid": "12345678-90ab-cdef-1234-567890abcdee",
    "description": "电子通行证最小示例程序。",
    "executable": {
        "file": "template"
    },
    "type": "fg",
    "screens": ["360x640"]
}
```

文件关联（`fg_ext`）：

```json
{
    "version": 1,
    "name": "文本阅读器",
    "uuid": "e11d3bc0-044a-49c5-9a96-f6fb5758c4d9",
    "description": "一个可以阅读文本文件的APP",
    "executable": { "file": "textreader" },
    "type": "fg_ext",
    "extensions": [".log", ".txt"],
    "screens": ["360x640"],
    "icon": "icon.png"
}
```

后台 IPC：

```json
{
    "version": 1,
    "name": "后台IPC应用测试",
    "uuid": "a6e3e6c7-4d89-4a0a-9cc1-8c4cf6a9b7e4",
    "description": "后台IPC应用测试，演示所有IPC请求。",
    "executable": { "file": "ipc_client_demo" },
    "type": "bg",
    "screens": ["360x640", "480x854", "720x1280"]
}
```

### 1.6 启动约定

- 菜单启动：`./executable_file`（cwd = 应用目录）
- 文件关联：`./executable_file /abspath/to/file`，在 `main` 里用 `argc > 1` 取路径
- 前台应用接管显示前，主程序会关闭 DRM master；应用 `drm_warpper_init` 时再 `drmSetMaster`
- 建议处理 `SIGINT` / `SIGTERM` 做资源清理；`KEY_4` 常作退出键

---

## 2. 图形接口（纯 libdrm）

### 2.1 接口真相源

最新封装：

- 头文件：`../drm_app_neo/src/driver/drm_warpper.h`
- 实现：`../drm_app_neo/src/driver/drm_warpper.c`

**已废弃（勿再用于新应用）：**

- `c_example/lib/srgn_drm.h`
- `drm_warpper_queue_item_t` 里的 `mount.arg0`（用户态指针 + `DRM_IOCTL_SRGN_ATOMIC_COMMIT`）
- `drm_warpper_reset_cache_ioctl`

提交路径改为标准 **`drmModeAtomicCommit`**：dumb buffer → `AddFB` → plane `FB_ID` 翻页。

### 2.2 与旧 API 的关键差异

| 点 | 旧（`c_example` / srgn） | 新（`drm_app_neo` / 纯 libdrm） |
|----|--------------------------|--------------------------------|
| 提交 | 自定义 ioctl，`arg0` = 用户态 `vaddr` | `drmModeAtomicCommit`，`fb_id` 换页 |
| queue item | `struct drm_srgn_atomic_commit_data mount` | `type` + `fb_id` / `x,y` / `alpha` |
| 翻页 | `MOUNT_FB_NORMAL` + `mount.arg0` | `DRM_WARPPER_ITEM_FLIP_FB` + `fb_id` |
| 绘图目标 | `(uint32_t*)curr_item->mount.arg0` | `buffer_object_t.vaddr`（dumb mmap） |
| Master | 简单 open | `drmSetMaster`；需 `DRM_CLIENT_CAP_ATOMIC` |

### 2.3 核心类型（摘要）

```c
typedef enum {
    DRM_WARPPER_LAYER_MODE_RGB565,
    DRM_WARPPER_LAYER_MODE_ARGB8888,
    DRM_WARPPER_LAYER_MODE_MB32_NV12, // Allwinner 专用（视频）
} drm_warpper_layer_mode_t;

typedef enum {
    DRM_WARPPER_ITEM_FLIP_FB,    // 用 fb_id 翻页
    DRM_WARPPER_ITEM_SET_COORD,  // CRTC_X/Y
    DRM_WARPPER_ITEM_SET_ALPHA,  // plane alpha；255 = 不透明（ARGB 回到像素 alpha）
} drm_warpper_item_type_t;

typedef struct {
    uint32_t width, height, pitch, handle, size;
    uint8_t *vaddr;
    uint32_t fb_id;
} buffer_object_t;

typedef struct {
    drm_warpper_item_type_t type;
    uint32_t fb_id;   // FLIP_FB
    int16_t x, y;     // SET_COORD
    uint8_t alpha;    // SET_ALPHA
    void *userdata;
    bool on_heap;
} drm_warpper_queue_item_t;
```

`buffer_object_t` / `drm_warpper_queue_item_t` 必须是 `static` 或生命周期覆盖整个运行期，禁止局部栈变量。

### 2.4 主程序图层约定（参考）

启动器内部层分配（第三方前台应用独占显示时可自选层）：

| 宏 | ID | 典型格式 |
|----|-----|----------|
| `DRM_WARPPER_LAYER_VIDEO` | 0 | MB32 NV12 |
| `DRM_WARPPER_LAYER_OVERLAY` | 1 | ARGB8888 |
| `DRM_WARPPER_LAYER_UI` | 2 | RGB565 |

硬件共 4 层（0–3），ID 越大优先级越高。全系统同时只能有一个透明层。前台第三方应用建议从层 `1` 起用 `ARGB8888`（与 `fbdraw` 一致）。

### 2.5 推荐生命周期

```
drm_warpper_init
  → init_layer(layer, w, h, ARGB8888)
  → allocate_buffer ×1 或 ×2
  → mount_layer（首次 modeset）
  → 循环绘制 / present
  → destroy
```

#### 方式 A：双缓冲 flip（适合动画/游戏）

```c
static buffer_object_t buf_1, buf_2;
static drm_warpper_queue_item_t item_1, item_2;

drm_warpper_allocate_buffer(&dw, layer, &buf_1);
drm_warpper_allocate_buffer(&dw, layer, &buf_2);

item_1.type = DRM_WARPPER_ITEM_FLIP_FB;
item_1.fb_id = buf_1.fb_id;
item_1.userdata = &buf_1;
item_1.on_heap = false;

item_2.type = DRM_WARPPER_ITEM_FLIP_FB;
item_2.fb_id = buf_2.fb_id;
item_2.userdata = &buf_2;
item_2.on_heap = false;

drm_warpper_mount_layer(&dw, layer, 0, 0, &buf_1);
drm_warpper_enqueue_display_item(&dw, layer, &item_1);
drm_warpper_enqueue_display_item(&dw, layer, &item_2);

drm_warpper_queue_item_t *curr = NULL;
while (running) {
    drm_warpper_dequeue_free_item(&dw, layer, &curr);
    buffer_object_t *buf = (buffer_object_t *)curr->userdata;
    uint32_t *vaddr = (uint32_t *)buf->vaddr;

    fbdraw_fb_t fb = { .vaddr = vaddr, .width = 360, .height = 640 };
    fbdraw_fill_rect(&fb, &(fbdraw_rect_t){0, 0, 360, 640}, 0xFF000000);

    curr->type = DRM_WARPPER_ITEM_FLIP_FB;
    curr->fb_id = buf->fb_id;
    drm_warpper_enqueue_display_item(&dw, layer, curr);
}
```

注意：

- 绘图目标是 **`buf->vaddr`**，不是旧的 `curr_item->mount.arg0`
- 提交时填 **`fb_id`**，不要传用户态指针
- ARGB8888 且 plane alpha=255 时走像素 alpha；不透明像素 alpha 须为 `0xFF`（如黑色 `0xFF000000`）
- `pitch` 可能有对齐；按行拷贝时使用 `buf->pitch`

#### 方式 B：单缓冲直绘（主 UI / Overlay 当前做法）

```c
static buffer_object_t buf;
drm_warpper_allocate_buffer(&dw, layer, &buf);
memset(buf.vaddr, 0, buf.size);
drm_warpper_mount_layer(&dw, layer, 0, 0, &buf);
// 之后直接写 buf.vaddr，不再走 flip 队列（可能轻微撕裂，省内存）
```

### 2.6 其它 API

| 函数 | 用途 |
|------|------|
| `allocate_buffer_sized` | 非层尺寸的 dumb buffer |
| `mount_layer_rect` | 裁切/缩放挂载（缩放仅 MB32_NV12 视频层） |
| `set_layer_coord` / `set_layer_alpha` | 位置与平面透明度 |
| `import_dmabuf_fb` / `rm_fb` | 视频 dmabuf 导入 |
| `set_layer_geometry` / `disable_layer_sync` | 视频层惰性挂载 / 同步关闭 |

普通第三方 GUI 一般只需：`init` / `init_layer` / `allocate_buffer` / `mount_layer` / flip 队列 / `destroy`。

---

## 3. 后台应用与 IPC

后台应用（`type: bg`）**不要**抢 DRM。通过 Unix socket 连接主程序：

- 路径：`/tmp/epass_drm_app.sock`
- 客户端头：`c_example/lib/ipc_client.h`、`ipc_common.h`
- 演示：`c_example/examples/ipc_client_demo/`

能力概览：UI 弹窗/切屏、PRTS、Settings、MediaPlayer、Overlay 过渡、UIX 确认会话、`ipc_client_app_exit`。

退出码（`ipc_common.h`）：`NORMAL=0`、`RESTART_APP=1`、`APPSTART=2`、`SHUTDOWN=3`、`FORMAT_SD_CARD=4`、`SRGN_CONFIG=5`。

写 PRTS / Mediaplayer / Overlay 前建议 `prts_set_blocked_auto_switch(true)` 避免竞态。

---

## 4. 硬件与输入（摘要）

- 屏：纵向，默认 360×640，约 60fps，DRM
- 按键：`KEY_1` 上、`KEY_2` 下、`KEY_3` 确定、`KEY_4` 退出；`keyinput_get_key()`，无键返回 `-1`
- 外设（见 `c_example/lib/epass_define.h`）：
  - I2C `/dev/i2c-0`
  - SPI `/dev/spidev1.0`
  - UART `/dev/ttyS1`、`/dev/ttyS2`
  - GPIO `gpiochip0`

绘图辅助仍可用 `fbdraw` / `fbdrawttf` / RREFont（CPU 画到 `vaddr`，不是 `/dev/fb`）。

---

## 5. 本仓库开发流程

1. 在本仓库下新建应用目录（或从 `c_example/examples/template` 拷贝思路）
2. 编写 `appconfig.json`（新 UUID、`type`、`screens`、`executable.file`）
3. 图形代码按 **第 2 节纯 libdrm API** 编写，不要复制 `c_example` 里 `mount.arg0` 旧写法
4. 将产物安装到设备 `/app/<name>/`（含 `appconfig.json` 与二进制）
5. 重启启动器或重新扫描后，在应用列表中验证

CMake 安装约定示例：

```cmake
install(TARGETS ${APPLICATION_NAME}
        DESTINATION ${CMAKE_INSTALL_PREFIX}/${APPLICATION_NAME})
install(FILES appconfig.json
        DESTINATION ${CMAKE_INSTALL_PREFIX}/${APPLICATION_NAME})
# 可选：icon、字体、assets
```

### 参考例程（`c_example/examples/`）

| 例程 | 说明 | 注意 |
|------|------|------|
| `template` | 最小前台模板 | 注册格式可直接参考 |
| `epniccc` | 按键 + 绘图 + RREFont | 图形提交仍是旧 srgn 写法，移植时改成 `FLIP_FB` |
| `textreader` | `fg_ext` + TTF | 同上 |
| `ipc_client_demo` | 后台 IPC 全覆盖 | 图形无关 |
| `i2c_test` / `spi_test` / `uart_test` / `libgpio_test` | 外设 | — |

---

## 6. 给 Agent 的硬约束

1. 注册只认同目录 `appconfig.json`，`version` 必须为 `1`，`uuid` / `executable` / `type` / `screens` 不可缺。
2. 新图形代码必须以 `drm_app_neo` 的 `drm_warpper` 为准：翻页用 `DRM_WARPPER_ITEM_FLIP_FB` + `fb_id`，绘制用 `buffer_object_t.vaddr`。
3. 禁止在新代码中引入 `srgn_drm.h`、`mount.arg0`、`DRM_IOCTL_SRGN_*`、`drm_warpper_reset_cache_ioctl`。
4. `fg` 应用可独占 DRM；`bg` 应用走 IPC，不要初始化显示。
5. 默认只保证 `360x640`，除非用户要求多分辨率。
