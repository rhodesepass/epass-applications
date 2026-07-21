# 云端 agent + wasm 实时预览:分阶段落地方案

> **2026-07-22 方向转折**:本文的 P0(独立编译服务)/P1(Agent SDK 沙箱)拆分已被
> 更薄的 devbox 形态取代——一个容器装 Claude Code CLI + emcc + buildroot 交叉
> 工具链,见 `cloud/devbox/README.md`。核心场景也从"改现有应用"改为**生成新应用**
> (双产物:wasm 预览 + 设备安装包)。本文保留作 P2(多租户/隔离/成本)的调研参考,
> 那部分结论(Firecracker/E2B 取舍、token 成本模型)仍然有效。

目标:web 站点,用户提需求 → 云端沙箱里的 AI agent 改本仓库代码 → emcc 编成 wasm →
浏览器里直接玩。类似"云版 Claude Code + 实时预览"。

前提(已具备,决定了方案可以很薄):

- HAL 双后端已验证,5 个游戏可用 `tools/build_wasm_game.sh` 一条 emcc 命令编出 wasm。
- 单次编译只有十几个 .c 文件,秒级完成;不是 Chromium 那种要分布式编译的量级。
- wasm 产物在**用户浏览器的沙箱**里跑,服务端不执行它。服务端要防的只有两类东西:
  编译器/构建脚本被喂了恶意输入,以及(P1 起)agent 自己在沙箱里跑的任意命令。

结论先行:P0 一周内能上线一个"贴 git ref 就能在线玩"的服务;P1 的核心是把
Claude Agent SDK 塞进容器并管好出网;P2 在用户量真的上来之前大部分可以不做。

---

## P0:最小"编译 + serve"服务

一台 VPS(2C4G 足够),一个 HTTP 服务,三个接口:

```
POST /build   {git_ref 或 源码 tarball}  → {build_id, 状态}
GET  /status/{build_id}                  → 编译日志(轮询或 SSE)
GET  /play/{build_id}/index.html         → 静态托管产物
```

流程:收到请求 → `git fetch` 到裸仓库缓存 + checkout 到临时目录 → 在容器里跑
`build_wasm_game.sh` → 产物拷到 `artifacts/{build_id}/` → 返回 URL。

### 技术选型

| 部件 | 选择 | 理由 |
|---|---|---|
| 服务本体 | 任意熟悉的语言写个 ~500 行的 HTTP 服务(Go/Python/Node 均可) | 就是个带队列的 CGI,别引框架 |
| 容器 | 普通 docker/podman,`--network none --read-only --memory 2g --pids-limit 256 --cpus 2` + 20s 超时 | Compiler Explorer 用的是更轻的 nsjail(namespace + seccomp,类似不带镜像管理的容器),但我们已有 docker 就不必换 |
| emsdk 镜像 | 官方 `emscripten/emsdk`,在其上 `FROM` 一层:预热 sysroot 缓存(emcc 编一次 hello world 让 libc/libcxx 落进 `upstream/emscripten/cache`)、装 ccache | 首次冷编译要现编 libc,几十秒;预热后回到秒级 |
| 编译缓存 | 镜像内预热缓存 + 挂一个只读共享 ccache 卷 | 我们的编译单元少,ccache 是锦上添花,预热 sysroot 才是关键 |
| 产物存储 | 本地磁盘目录,nginx/caddy 直接 serve,7 天定时清理 | 每个游戏产物 <2MB,一天 1000 次编译也就 2GB |
| 去重 | 对 (commit hash + app 名) 做 key,命中直接返回旧 URL | 内容寻址,同 Compiler Explorer 的 S3 缓存思路,只是存本地 |

### 参考:Compiler Explorer 怎么做的

公开资料([How Compiler Explorer Works in 2025](https://xania.org/202506/how-compiler-explorer-works)):
每实例最多 2 个并发编译、nsjail 沙箱(全套 namespace + 资源限制 + 无网络)、20 秒超时、
三层缓存(浏览器 / 实例内 LRU / S3 内容寻址)、按 CPU 负载自动扩容。年 9200 万次编译、
月成本约 $3000。**对我们的启示是反向的**:它的复杂度来自 3.9TB 的编译器集合和公网流量,
我们只有一个 emcc 和一个仓库,它的架构里 90% 都不需要,抄它的"沙箱 + 超时 + 内容寻址缓存"
三件事就够。

### 工作量 / 风险 / 可砍

- **工作量:3~5 人日**(HTTP 服务 2 天、Dockerfile + 预热 1 天、部署 + 清理脚本 1 天)。
- **最大风险**:恶意/畸形输入让 emcc 挂死或撑爆磁盘 → 超时 + `--network none` +
  tmpfs 限额基本兜住;P0 阶段干脆只接受 git ref 白名单(只允许本仓库 + fork),不收任意 tarball。
- **可砍**:SSE 日志(先轮询)、ccache(预热镜像已够快)、去重缓存(量小时无所谓)。

---

## P1:加 agent 循环

在 P0 的编译沙箱旁边加一种"agent 沙箱":容器里跑 Claude Agent SDK(它会拉起一个
claude CLI 子进程,通过 stdio 通信——可以理解成 SDK 是个进程监督器,agent 的 shell、
工作目录、会话记录都在这个容器里)。用户网页 ↔ 我们的服务 ↔ 容器内 SDK 进程。

### 部署形态(照官方 hosting 文档抄即可)

官方对自托管的定型([Hosting the Agent SDK](https://code.claude.com/docs/en/agent-sdk/hosting),
配套 [hosting cookbook](https://github.com/anthropics/claude-cookbooks/tree/main/claude_agent_sdk/hosting)
有现成 Dockerfile/K8s/Modal 三套可跑代码):

- 一个会话 = 一个 claude 子进程 = 一个容器。**每 agent 预算 1 GiB RAM / 5 GiB 盘 / 1 CPU**。
- 会话模式选"**混合式**":容器按需拉起,会话记录(JSONL transcript)通过 SDK 的
  SessionStore 适配器(官方有 S3/Redis/Postgres 参考实现)存到容器外,用户回来时
  `resume: sessionId` 恢复。闲置即销毁容器,不烧钱。
- 容器里只需 Node 18+ 或 Python 3.10+,SDK 包自带 claude 二进制,无需单独装 Claude Code。
- API key 不放容器里:设 `ANTHROPIC_BASE_URL` 指向宿主机上的小代理,由代理注入 key。
  类比:key 是保险丝,装在机箱外面,箱内短路烧不到它。

### 出网白名单

编译沙箱维持 `--network none`。agent 沙箱需要出网,两档做法:

1. **简单档(P1 先用)**:Anthropic 官方的
   [sandbox-runtime](https://github.com/anthropic-experimental/sandbox-runtime)
   (`@anthropic-ai/sandbox-runtime`,Linux 下用 bubblewrap + 内建代理),JSON 配置
   允许域名:`api.anthropic.com`、`github.com`、`codeload.github.com`、
   `objects.githubusercontent.com`(git 走 HTTPS,不开 ssh)。
   已知弱点:代理按客户端声称的域名放行、不解 TLS,理论上可被 domain fronting 绕过——
   对"防 agent 手滑"够用,对"防蓄意攻击者"不够。
2. **加固档(P2 再上)**:容器 `--network none`,只挂一个 Unix socket 连到宿主机上的
   TLS 终结代理(Envoy/mitmproxy/Squid),真正的域名白名单 + 全量审计日志。这是官方
   [secure-deployment 文档](https://code.claude.com/docs/en/agent-sdk/secure-deployment)
   推荐的模式,也是 Claude Code 自家沙箱的架构。

### 会话通道

用户浏览器到服务端用 **SSE**(单向下行流,agent 输出天然是流式的;用户输入走普通 POST)。
比 WebSocket 省事:纯 HTTP、过任何反代、断线重连浏览器自带。服务端把 SDK `query()`
吐出的 message 流原样转发,再加两种自定义事件:`build_done {url}`(iframe 刷新预览)、
`need_input`(权限询问,如果不用全自动模式)。

agent 的工具配置:关掉交互式权限询问(headless 原则:`allowed_tools` 精确放行
Read/Edit/Write/Bash,`can_use_tool` 回调兜底自动拒绝,绝不留会卡住等人的口子),
给它一个 `build_and_preview` 自定义工具——内部就是调 P0 的编译服务,把编译日志喂回
agent,形成"改代码 → 编译 → 看报错 → 再改"的循环。**P0 的编译服务因此不是过渡产物,
而是 P1 的一个工具后端。**

### 工作量 / 风险 / 可砍

- **工作量:8~12 人日**(容器镜像 + SDK 集成 2 天、SSE 通道 + 前端聊天页 3 天、
  出网代理 1 天、会话持久化 2 天、agent 提示词与工具打磨 2~4 天)。
- **最大风险**:不是技术是**成本**——容器约 $0.05/小时,但一次长 agent 会话的 token
  能烧几美元,token 费比机器费高一个数量级。必须从第一天就做 per-session token 记账
  + `maxTurns` 上限。次要风险:SDK 无内建会话超时,内存随会话增长,要主动回收长会话。
- **可砍**:SessionStore(先接受"容器死了会话就丢",单机 demo 完全可接受)、
  多游戏并行编辑(先限定一次改一个 app)、加固档代理。

---

## P2:多租户与隔离

### 威胁模型先想清楚

分层看谁在跑不可信代码:

| 东西 | 跑在哪 | 谁防 |
|---|---|---|
| wasm 产物 | 用户浏览器 | 浏览器沙箱,与我无关 |
| emcc 编译 | 编译容器 | 无网络 + 超时 + 资源限制,攻击面只有"编译器 bug 被恶意源码触发" |
| agent 执行的命令 | agent 容器 | 真正要防的:prompt injection 或用户蓄意让 agent 跑恶意 build 脚本 |

关键点:agent 容器里的代码是"半可信"(Claude 生成 + 用户引导),最坏情况是有人
把这当免费矿机或跳板。要防的是**容器逃逸**和**滥用出网**,不是防它弄坏容器内部——
容器本来就是一次性的。

### 隔离技术取舍

| 方案 | 隔离强度 | 开销 | 判断 |
|---|---|---|---|
| docker + seccomp + `--cap-drop ALL` + no-new-privileges | 共享宿主内核,内核洞即逃逸 | 最低 | 单机自用/内测够;多租户收费服务不够 |
| gVisor(runsc) | 用户态内核拦截 syscall,攻击面小得多 | 文件 I/O 重的场景可慢 10~200 倍,而编译恰好是 open/close 密集型 | **对编译负载是最差选择**,排除 |
| Firecracker microVM | 独立 guest 内核,硬件级边界 | 启动 <125ms、每实例 ~5MiB 额外内存,代价在启动不在运行时 | 多租户的正解;E2B/Fly 底层都是它 |

结论:**P0/P1 用加固 docker(cookbook 里那套 `--cap-drop ALL --read-only --tmpfs`
参数照抄),P2 若自建就换 Firecracker,若不想维护 KVM 宿主机就直接买 E2B/Fly**(见下节)。
gVisor 跳过。

### 生命周期与配额

- **闲置回收**:SSE 断开 + 10 分钟无输入 → 快照会话(SessionStore)→ 杀容器。
- **配额**:免费用户每天 N 次 agent 会话、每会话 token 上限、编译次数上限;
  编译服务对 IP 限流。
- **成本粗算**(20 个日活重度用户):机器费 20 会话·时/天 × $0.05 ≈ $30/月 + VPS $20;
  token 费 20 会话/天 × $1~3 ≈ **$600~1800/月,token 就是全部成本**,机器怎么选型
  都是零头。这也反过来说明:P2 的隔离投入应该等付费模式想清楚之后再花。

### 工作量 / 风险 / 可砍

- **工作量:10~15 人日**(Firecracker 或托管沙箱迁移 4 天、配额与计费记账 3 天、
  回收与监控(OTEL 环境变量即可导出)3 天、加固档出网代理 2 天)。
- **最大风险**:在没有付费用户前过度建设。token 成本模型不成立的话 P2 根本不该开工。
- **可砍**:几乎全部——单机 + 加固 docker + 邀请制(熟人可信)可以撑到几十个用户。

---

## 现实检查:自建 vs 借力

| 产品 | 能替我们做什么 | 不合适的点 |
|---|---|---|
| **E2B** | Firecracker 沙箱即服务,80ms 冷启动,~$0.05/vCPU·h,官方文档点名支持 Agent SDK;P2 的隔离 + 生命周期全包 | 编译镜像要做成它的自定义 template;深度绑定 |
| **Fly Machines / Sprites** | 持久化 Firecracker microVM,闲置不计费、文件系统保留——和"混合式会话"模式天然契合(4 小时 Claude Code 会话实测约 $0.44) | 冷启动 1~12s;规模大了偏贵 |
| **Modal** | 官方 hosting cookbook 有现成 Modal 部署示例,免费额度 $30/月,Python 定义镜像很舒服 | 按物理核计价偏贵;Python-only 的框架思维 |
| **GitHub Codespaces** | 面向"人开 IDE",按需 agent 沙箱这种 API 驱动场景不顺手 | 排除 |
| **WebContainers**(StackBlitz) | 在浏览器里跑 Node —— 但 emcc 是原生 LLVM + Python,根本跑不进去 | 排除,除非哪天 emcc 自身有 wasm 发行版 |
| **Managed Agents**(Anthropic 托管) | Anthropic 代跑 agent + 沙箱,REST 接口,零运维 | 沙箱内环境可控性待验证(能否装 emsdk、出网策略);值得做半天 spike |

**建议路线**:P0/P1 全自建(单机 docker,总共两周内的活,借力反而增加集成面);
P2 到来时**默认买 E2B 或 Fly 而不是自建 Firecracker**——自建省下的机器费(每月几十刀)
远抵不上维护 KVM 宿主机的时间,而且如上所述成本大头在 token,沙箱费怎么选都无所谓。

---

## 总时间线

| 阶段 | 人日 | 里程碑 |
|---|---|---|
| P0 | 3~5 | 贴 git ref → 30 秒后拿到可玩 URL |
| P1 | 8~12 | 网页聊天 → agent 改 snake → iframe 里立刻能玩 |
| P2 | 10~15(可无限推迟) | 邀请制之外的多租户开放 |

主要参考:[Compiler Explorer 2025 架构](https://xania.org/202506/how-compiler-explorer-works) ·
[Agent SDK Hosting](https://code.claude.com/docs/en/agent-sdk/hosting) ·
[Secure Deployment](https://code.claude.com/docs/en/agent-sdk/secure-deployment) ·
[sandbox-runtime](https://github.com/anthropic-experimental/sandbox-runtime) ·
[hosting cookbook](https://github.com/anthropics/claude-cookbooks/tree/main/claude_agent_sdk/hosting) ·
[Firecracker vs gVisor](https://northflank.com/blog/firecracker-vs-gvisor) ·
[沙箱服务商价格对比](https://northflank.com/blog/ai-sandbox-pricing)(数据截至 2026-07)
