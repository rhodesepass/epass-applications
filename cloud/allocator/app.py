#!/usr/bin/env python3
"""epass web 试用分配器: 单进程干四件事。

1. 公网 listener(:8000): 首页发会话 → docker 拉 devbox 容器;
   /s/<token>/ide|preview 反代(HTTP+WS)到容器 IP, token 即访问凭据。
2. LLM listener(unix socket, 挂载进容器): 给容器里的 claude 注入真
   API key 转发到企业转发 URL。不走 TCP, 外网碰不到, 也不依赖宿主防火墙
   放行 bridge 流量; 容器内 llm_forward.py 把 127.0.0.1:9100 接到 socket。
3. reaper: 60 分钟无活动销毁容器。
4. 归档: 销毁前把 workspace 兜底 commit + push 到 hub.git 的
   trial/<会话名> 分支, 代码不随容器消失。

session 表是内存 dict, 分配器重启即清场(残留容器先归档再删)。
"""

import asyncio
import contextlib
import logging
import os
import secrets
import shutil
import signal
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

import httpx
import uvicorn
import websockets
from fastapi import FastAPI, Request, WebSocket
from starlette.background import BackgroundTask
from starlette.responses import (HTMLResponse, RedirectResponse, Response,
                                 StreamingResponse)

log = logging.getLogger("allocator")

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "cloud" / "data"
HUB = DATA / "hub.git"
WS_BASE = DATA / "workspaces"
STATIC = Path(__file__).resolve().parent / "static"

LLM_KEY = os.environ.get("EPASS_LLM_KEY", "")
LLM_UPSTREAM = os.environ.get("EPASS_LLM_UPSTREAM", "").rstrip("/")
MAX_SESSIONS = int(os.environ.get("EPASS_MAX_SESSIONS", "8"))
PUBLIC_PORT = int(os.environ.get("PUBLIC_PORT", "8000"))
LLM_SOCK_DIR = DATA / "llmsock"
LLM_SOCK = LLM_SOCK_DIR / "llm.sock"
IDLE_TIMEOUT = int(os.environ.get("IDLE_TIMEOUT", "3600"))
IMAGE = os.environ.get("EPASS_IMAGE", "epass-devbox")

GIT_ID = ["-c", "user.name=epass-trial", "-c", "user.email=trial@epass.local"]
PANEL_PORTS = {"term": 7681, "ide": 8443, "preview": 8080}
HOP_HEADERS = {"connection", "keep-alive", "proxy-authenticate",
               "proxy-authorization", "te", "trailers", "transfer-encoding",
               "upgrade"}
METHODS = ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"]


@dataclass
class Session:
    token: str
    sid: str            # 短名: 容器 epass-web-<sid>, 分支 trial/<sid>
    ip: str
    ws_dir: Path        # cloud/data/workspaces/web-<sid>
    base_commit: str    # clone 时的 HEAD, 归档时没前进就不 push
    last_active: float = field(default_factory=time.monotonic)

    def touch(self):
        self.last_active = time.monotonic()


sessions: dict[str, Session] = {}
create_times: deque[float] = deque(maxlen=8)
client: httpx.AsyncClient | None = None


async def run(*cmd: str) -> tuple[int, str]:
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    out, _ = await proc.communicate()
    return proc.returncode, out.decode(errors="replace").strip()


def page(name: str, status: int = 200, **subst: str) -> HTMLResponse:
    html = (STATIC / name).read_text()
    for k, v in subst.items():
        html = html.replace("{{%s}}" % k, v)
    return HTMLResponse(html, status_code=status)


# ---------------- 会话编排 ----------------

async def create_session() -> Session:
    token = secrets.token_urlsafe(16)
    sid = token[:8]
    ws_dir = WS_BASE / f"web-{sid}"
    repo = ws_dir / "repo"
    ws_dir.mkdir(parents=True)

    for cmd in (["git", "clone", str(HUB), str(repo)],
                ["git", "-C", str(repo), "checkout", "-b", f"trial/{sid}"],
                ["git", "-C", str(repo), "config", "user.name", "epass-trial"],
                ["git", "-C", str(repo), "config", "user.email",
                 "trial@epass.local"]):
        rc, out = await run(*cmd)
        if rc != 0:
            shutil.rmtree(ws_dir, ignore_errors=True)
            raise RuntimeError(f"workspace 准备失败: {' '.join(cmd)}: {out}")
    _, base = await run("git", "-C", str(repo), "rev-parse", "HEAD")

    docker = ["docker", "run", "-d", "--name", f"epass-web-{sid}",
              "-e", f"ANTHROPIC_BASE_URL=http://127.0.0.1:9100/llm/{token}",
              "-e", "ANTHROPIC_API_KEY=proxy",  # 占位, 真 key 由代理注入
              "--memory", "3g", "--cpus", "2", "--pids-limit", "512",
              "-v", f"{repo}:/workspace",
              "-v", f"{LLM_SOCK_DIR}:/run/epass"]
    for var in ("ANTHROPIC_MODEL", "ANTHROPIC_SMALL_FAST_MODEL"):
        if os.environ.get(var):
            docker += ["-e", f"{var}={os.environ[var]}"]
    docker += [IMAGE, "/usr/local/bin/entrypoint-web.sh"]
    rc, out = await run(*docker)
    if rc != 0:
        shutil.rmtree(ws_dir, ignore_errors=True)
        raise RuntimeError(f"docker run 失败: {out}")

    # 顶层 .NetworkSettings.IPAddress 在 docker 26+ 已移除, 走 Networks map
    rc, ip = await run("docker", "inspect", "-f",
                       "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}",
                       f"epass-web-{sid}")
    if rc != 0 or not ip:
        await run("docker", "rm", "-f", f"epass-web-{sid}")
        shutil.rmtree(ws_dir, ignore_errors=True)
        raise RuntimeError(f"拿不到容器 IP: {ip}")

    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            r = await client.get(f"http://{ip}:8443/healthz", timeout=2)
            if r.status_code == 200:
                break
        except httpx.HTTPError:
            pass
        await asyncio.sleep(0.5)
    else:
        await run("docker", "rm", "-f", f"epass-web-{sid}")
        shutil.rmtree(ws_dir, ignore_errors=True)
        raise RuntimeError("code-server 30s 内没起来")

    s = Session(token=token, sid=sid, ip=ip, ws_dir=ws_dir, base_commit=base)
    sessions[token] = s
    log.info("会话 %s 就绪: 容器 epass-web-%s @ %s", sid, sid, ip)
    return s


async def archive_workspace(sid: str, ws_dir: Path, base_commit: str = ""):
    """兜底 commit + push trial/<sid> 到 hub。失败只记日志。"""
    repo = ws_dir / "repo"
    if not (repo / ".git").exists():
        return
    rc, dirty = await run("git", "-C", str(repo), "status", "--porcelain")
    if rc == 0 and dirty:
        await run("git", "-C", str(repo), "add", "-A")
        rc, out = await run("git", *GIT_ID, "-C", str(repo), "commit",
                            "-m", "trial session auto-archive")
        if rc != 0:
            log.warning("归档 commit 失败 %s: %s", sid, out)
    _, head = await run("git", "-C", str(repo), "rev-parse", "HEAD")
    if head and head != base_commit:
        # origin 就是 hub(clone 来源); orphan 目录归档时显式指 hub 更稳
        rc, out = await run("git", "-C", str(repo), "push", str(HUB),
                            f"HEAD:refs/heads/trial/{sid}")
        if rc != 0:
            log.warning("归档 push 失败 %s: %s", sid, out)
        else:
            log.info("会话 %s 已归档到 trial/%s", sid, sid)


async def destroy_session(token: str):
    s = sessions.pop(token, None)
    if not s:
        return
    await run("docker", "rm", "-f", f"epass-web-{s.sid}")
    await archive_workspace(s.sid, s.ws_dir, s.base_commit)
    shutil.rmtree(s.ws_dir, ignore_errors=True)
    log.info("会话 %s 已销毁", s.sid)


async def reaper():
    while True:
        await asyncio.sleep(60)
        now = time.monotonic()
        for token in [t for t, s in sessions.items()
                      if now - s.last_active > IDLE_TIMEOUT]:
            log.info("会话 %s 闲置超时", sessions[token].sid)
            with contextlib.suppress(Exception):
                await destroy_session(token)


async def startup_cleanup():
    """重启清场: 残留容器/workspace 先归档再删。"""
    rc, out = await run("docker", "ps", "-aq", "--filter", "name=epass-web-")
    for cid in out.split():
        await run("docker", "rm", "-f", cid)
    # 归档统一在 workspace 目录这一遍做(孤儿目录也覆盖)
    if WS_BASE.exists():
        for d in WS_BASE.glob("web-*"):
            sid = d.name.removeprefix("web-")
            await archive_workspace(sid, d)
            shutil.rmtree(d, ignore_errors=True)


# ---------------- 反代 ----------------

def gone(token: str = "") -> HTMLResponse:
    return page("gone.html", status=410)


async def proxy_http(request: Request, ip: str, port: int, prefix: str,
                     path: str) -> Response:
    url = f"http://{ip}:{port}/{path}"
    if request.url.query:
        url += "?" + request.url.query
    headers = {k: v for k, v in request.headers.items()
               if k.lower() not in HOP_HEADERS and k.lower() != "host"}
    # 整读 body 再转发: 流式空 body 会被 httpx 发成 chunked, ttyd 的
    # libwebsockets 不吃 GET+chunked; bytes 则自带 content-length
    req = client.build_request(request.method, url, headers=headers,
                               content=await request.body())
    try:
        up = await client.send(req, stream=True)
    except httpx.HTTPError:
        return gone()
    resp_headers = {k: v for k, v in up.headers.items()
                    if k.lower() not in HOP_HEADERS}
    # 上游(python http.server / code-server)的绝对路径跳转要改写回前缀下
    loc = resp_headers.get("location", "")
    if loc.startswith("/"):
        resp_headers["location"] = prefix + loc
    return StreamingResponse(up.aiter_raw(), status_code=up.status_code,
                             headers=resp_headers,
                             background=BackgroundTask(up.aclose))


def make_public_app() -> FastAPI:
    app = FastAPI(openapi_url=None, docs_url=None, redoc_url=None)

    @app.get("/")
    async def index():
        return page("index.html", NOTICE="")

    @app.post("/new")
    async def new():
        now = time.monotonic()
        recent = sum(1 for t in create_times if now - t < 60)
        if recent >= 3:
            return HTMLResponse("创建太频繁, 稍等一分钟再试。", status_code=429)
        if len(sessions) >= MAX_SESSIONS:
            return page("index.html", status=503,
                        NOTICE="当前满员, 请稍后再试。")
        create_times.append(now)
        try:
            s = await create_session()
        except RuntimeError as e:
            log.error("%s", e)
            return HTMLResponse("会话创建失败, 请稍后再试。", status_code=500)
        return RedirectResponse(f"/s/{s.token}/", status_code=302)

    @app.get("/s/{token}/")
    async def session_page(token: str):
        s = sessions.get(token)
        if not s:
            return gone(token)
        s.touch()
        return page("session.html", TOKEN=token)

    # 相对路径解析要求前缀以 / 结尾
    @app.get("/s/{token}/{panel}")
    async def add_slash(request: Request, token: str, panel: str):
        if panel not in PANEL_PORTS:
            return gone(token)
        return RedirectResponse(str(request.url.path) + "/", status_code=301)

    @app.api_route("/s/{token}/{panel}/{path:path}", methods=METHODS)
    async def panel_http(request: Request, token: str, panel: str, path: str):
        s = sessions.get(token)
        port = PANEL_PORTS.get(panel)
        if not s or not port:
            return gone(token)
        s.touch()
        return await proxy_http(request, s.ip, port,
                                f"/s/{token}/{panel}", path)

    @app.websocket("/s/{token}/{panel}/{path:path}")
    async def panel_ws(ws: WebSocket, token: str, panel: str, path: str):
        s = sessions.get(token)
        port = PANEL_PORTS.get(panel)
        if not s or not port:
            await ws.close(code=1008)
            return
        s.touch()
        url = f"ws://{s.ip}:{port}/{path}"
        if ws.url.query:
            url += "?" + ws.url.query
        proto = ws.headers.get("sec-websocket-protocol")
        subprotocols = [p.strip() for p in proto.split(",")] if proto else None
        try:
            async with websockets.connect(url, subprotocols=subprotocols,
                                          max_size=None) as up:
                await ws.accept(subprotocol=up.subprotocol)

                async def client_to_upstream():
                    while True:
                        msg = await ws.receive()
                        if msg["type"] == "websocket.disconnect":
                            return
                        s.touch()
                        if msg.get("text") is not None:
                            await up.send(msg["text"])
                        elif msg.get("bytes") is not None:
                            await up.send(msg["bytes"])

                async def upstream_to_client():
                    async for m in up:
                        if isinstance(m, str):
                            await ws.send_text(m)
                        else:
                            await ws.send_bytes(m)

                tasks = [asyncio.create_task(client_to_upstream()),
                         asyncio.create_task(upstream_to_client())]
                _, pending = await asyncio.wait(
                    tasks, return_when=asyncio.FIRST_COMPLETED)
                for t in pending:
                    t.cancel()
        except (OSError, websockets.WebSocketException):
            with contextlib.suppress(Exception):
                await ws.close(code=1011)
        finally:
            with contextlib.suppress(Exception):
                await ws.close()

    return app


def make_llm_app() -> FastAPI:
    app = FastAPI(openapi_url=None, docs_url=None, redoc_url=None)

    @app.api_route("/llm/{token}/{path:path}", methods=METHODS)
    async def llm(request: Request, token: str, path: str):
        s = sessions.get(token)
        if not s:
            return Response("unknown session", status_code=403)
        s.touch()  # agent 长任务续命, 不靠浏览器活动
        url = f"{LLM_UPSTREAM}/{path}"
        if request.url.query:
            url += "?" + request.url.query
        headers = {k: v for k, v in request.headers.items()
                   if k.lower() not in HOP_HEADERS
                   and k.lower() not in ("host", "x-api-key", "authorization")}
        headers["x-api-key"] = LLM_KEY
        req = client.build_request(request.method, url, headers=headers,
                                   content=await request.body())
        try:
            up = await client.send(req, stream=True)  # SSE 必须流式
        except httpx.HTTPError as e:
            return Response(f"upstream error: {e}", status_code=502)
        resp_headers = {k: v for k, v in up.headers.items()
                        if k.lower() not in HOP_HEADERS}
        return StreamingResponse(up.aiter_raw(), status_code=up.status_code,
                                 headers=resp_headers,
                                 background=BackgroundTask(up.aclose))

    return app


# ---------------- main ----------------

class NoSignalServer(uvicorn.Server):
    """两个 Server 同跑一个 loop, 信号由 main 统一管, 别各自抢。"""

    def install_signal_handlers(self):
        pass

    @contextlib.contextmanager
    def capture_signals(self):
        yield


async def main():
    global client
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    if not LLM_KEY or not LLM_UPSTREAM:
        raise SystemExit("需要 EPASS_LLM_KEY / EPASS_LLM_UPSTREAM 环境变量")

    if not HUB.exists():
        rc, out = await run("git", "clone", "--bare", str(ROOT), str(HUB))
        if rc != 0:
            raise SystemExit(f"hub 初始化失败: {out}")
        log.info("hub 已初始化: %s", HUB)
    else:
        # 只刷 master, 不动 trial/* 归档分支; +强制, 主仓库 rebase 也能跟上
        rc, out = await run("git", "-C", str(HUB), "fetch", str(ROOT),
                            "+refs/heads/master:refs/heads/master")
        if rc != 0:
            log.warning("hub master 刷新失败: %s", out)
    WS_BASE.mkdir(parents=True, exist_ok=True)
    LLM_SOCK_DIR.mkdir(parents=True, exist_ok=True)
    LLM_SOCK.unlink(missing_ok=True)

    # SSE 长连接: 读不设超时
    client = httpx.AsyncClient(
        timeout=httpx.Timeout(connect=10, read=None, write=None, pool=None))

    await startup_cleanup()

    servers = [
        NoSignalServer(uvicorn.Config(make_public_app(), host="0.0.0.0",
                                      port=PUBLIC_PORT, log_level="warning")),
        NoSignalServer(uvicorn.Config(make_llm_app(), uds=str(LLM_SOCK),
                                      log_level="warning")),
    ]
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(
            sig, lambda: [setattr(srv, "should_exit", True)
                          for srv in servers])

    reap = asyncio.create_task(reaper())
    log.info("公网 http://0.0.0.0:%d  LLM 代理 %s (unix socket, 仅容器可达)",
             PUBLIC_PORT, LLM_SOCK)
    await asyncio.gather(*(srv.serve() for srv in servers))
    reap.cancel()
    await client.aclose()


if __name__ == "__main__":
    asyncio.run(main())
