"""MCP control plane; playback is direct Ogg HTTP, never arbitrary resources/read."""
import asyncio
import json
import logging
import os
import time
from collections import OrderedDict
from pathlib import Path
import websockets
from config import MCP_ENDPOINT, STREAM_PORT
from stream_proxy import StreamProxy
from youtube_provider import YouTubeProvider
from zingmp3_provider import ZingMP3Provider
from mcp_handler import MCPHandler

logger = logging.getLogger(__name__)

async def dispatch(data, handler):
    request_id = data.get('id')
    method = data.get('method')
    params = data.get('params', {})
    response = {'jsonrpc': '2.0', 'id': request_id}
    if not isinstance(params, dict):
        response['error'] = {'code': -32602, 'message': 'params must be an object'}
    elif method == 'initialize':
        response['result'] = {'protocolVersion': '2024-11-05', 'capabilities': {'tools': {}}, 'serverInfo': handler.get_server_info()}
    elif method == 'ping':
        response['result'] = {}
    elif method == 'tools/list':
        response['result'] = {'tools': handler.get_tools_list()}
    elif method == 'tools/call':
        response['result'] = await handler.handle_call_tool(params.get('name'), params.get('arguments', {}))
    else:
        response['error'] = {'code': -32601, 'message': 'Method not supported; use direct Ogg playback'}
    return response

async def serve_connection(ws, handler):
    active = {}
    completed = OrderedDict()
    send_lock = asyncio.Lock()
    async def send(response):
        async with send_lock:
            await ws.send(json.dumps(response, ensure_ascii=False))
    async def execute(data, fingerprint):
        rid = data['id']
        try:
            response = await dispatch(data, handler)
            completed[rid] = (time.monotonic(), fingerprint, response)
            while len(completed) > 64:
                completed.popitem(last=False)
            await send(response)
        except asyncio.CancelledError:
            raise
        except Exception:
            logger.warning('MCP request failed id=%s', rid)
        finally:
            active.pop(rid, None)
    try:
        async for raw in ws:
            try:
                data = json.loads(raw)
                if not isinstance(data, dict) or data.get('jsonrpc') != '2.0' or not isinstance(data.get('method'), str):
                    raise ValueError()
                rid = data.get('id')
                if rid is None: continue
                if isinstance(rid, bool) or not isinstance(rid, (str, int)): raise ValueError()
                fingerprint = json.dumps([data['method'], data.get('params', {})], sort_keys=True)
            except (ValueError, TypeError):
                await send({'jsonrpc':'2.0', 'id':None, 'error':{'code':-32600, 'message':'Invalid request'}})
                continue
            now = time.monotonic()
            for key in list(completed):
                if now - completed[key][0] > 120: completed.pop(key)
            previous = completed.get(rid)
            if previous:
                if previous[1] == fingerprint: await send(previous[2])
                else: await send({'jsonrpc':'2.0','id':rid,'error':{'code':-32600,'message':'Request id reused with different arguments'}})
                continue
            if rid in active:
                if active[rid][1] != fingerprint:
                    await send({'jsonrpc':'2.0','id':rid,'error':{'code':-32600,'message':'Request id collision'}})
                continue
            if len(active) >= 8:
                await send({'jsonrpc':'2.0','id':rid,'error':{'code':-32000,'message':'Too many requests'}})
                continue
            task = asyncio.create_task(execute(data, fingerprint))
            active[rid] = (task, fingerprint)
    finally:
        handler.cancel_pending()
        tasks = [row[0] for row in active.values()]
        for task in tasks: task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

async def run_server():
    if not MCP_ENDPOINT.startswith('wss://'):
        raise ValueError('Set MCP_ENDPOINT in the ignored root .env or environment')
    yt = YouTubeProvider()
    proxy = StreamProxy(STREAM_PORT)
    handler = MCPHandler(proxy, yt, ZingMP3Provider(yt))
    fleet = None
    if os.getenv("FLEET_ENABLED") == "1":
        from fleet import Fleet
        fleet = Fleet(Path(__file__).resolve().parents[1] / ".fleet-data", proxy, handler)
        fleet.attach(proxy.app)
    backoff = 2
    try:
        await proxy.start(private_routes=fleet is not None)
        if fleet:
            port = int(os.getenv("FLEET_PORT", "8766"))
            await fleet.start(port)
            logger.info("Fleet dashboard at http://127.0.0.1:%s", port)
        while True:
            try:
                async with websockets.connect(MCP_ENDPOINT, open_timeout=15, ping_interval=20,
                        ping_timeout=15, max_size=65536) as ws:
                    logger.info('MCP connected')
                    if fleet:
                        fleet.connected = True
                    backoff = 2
                    await serve_connection(ws, handler)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                logger.warning('MCP disconnected: %s; retry in %ss', type(exc).__name__, backoff)
            finally:
                if fleet:
                    fleet.connected = False
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)
    finally:
        handler.close()
        if fleet:
            await fleet.stop_admin()
        await proxy.stop()
        if fleet:
            fleet.db.close()

if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(name)s %(message)s')
    try:
        asyncio.run(run_server())
    except KeyboardInterrupt:
        pass
