#!/usr/bin/env python3
"""容器内 LLM 转发器: 127.0.0.1:9100 <-> 挂载进来的分配器 unix socket。

claude CLI 只会说 TCP, 分配器的 key 注入代理只听 unix socket(不碰宿主
防火墙), 中间差这一跳。纯 stdlib, 无依赖。
"""
import asyncio
import os

SOCK = os.environ.get("LLM_SOCK", "/run/epass/llm.sock")


async def handle(cr, cw):
    try:
        ur, uw = await asyncio.open_unix_connection(SOCK)
    except OSError:
        cw.close()
        return

    async def pump(r, w):
        try:
            while data := await r.read(65536):
                w.write(data)
                await w.drain()
        except OSError:
            pass
        finally:
            try:
                w.close()
            except OSError:
                pass

    await asyncio.gather(pump(cr, uw), pump(ur, cw))


async def main():
    srv = await asyncio.start_server(handle, "127.0.0.1", 9100)
    async with srv:
        await srv.serve_forever()


asyncio.run(main())
