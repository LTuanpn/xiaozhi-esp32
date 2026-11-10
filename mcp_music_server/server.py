import sys
import json
import base64
import logging
import asyncio
import aiohttp
import urllib.parse
import websockets
from config import MCP_ENDPOINT, LAN_IP, STREAM_PORT, STREAM_URL, FFMPEG_CMD
from stream_proxy import StreamProxy
from youtube_provider import YouTubeProvider
from zingmp3_provider import ZingMP3Provider
from mcp_handler import MCPHandler

# Đảm bảo mã hóa UTF-8 cho console tiếng Việt
if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if sys.stderr and hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

logging.basicConfig(
    level=logging.INFO,
    format='[%(asctime)s] [%(levelname)s] [%(name)s] %(message)s',
    datefmt='%H:%M:%S'
)
logger = logging.getLogger("XiaozhiMusicServer")

async def handle_resource_read(params: dict, session: aiohttp.ClientSession):
    """
    Xử lý phương thức 'resources/read' theo chuẩn Xiaozhi Cloud.
    Hỗ trợ đọc phân đoạn Base64 qua HTTP Range để truyền âm thanh xuống robot.
    """
    uri = params.get("uri", "")
    args = params.get("arguments", {})
    
    url = args.get("url")
    start = args.get("start", 0)
    end = args.get("end", 102400)

    # Nếu URL nằm trong query string của URI
    if not url and "?" in uri:
        parsed = urllib.parse.urlparse(uri)
        q = urllib.parse.parse_qs(parsed.query)
        if "url" in q:
            url = q["url"][0]
        if "start" in q:
            start = int(q["start"][0])
        if "end" in q:
            end = int(q["end"][0])

    if not url:
        url = STREAM_URL

    logger.info(f"Xiaozhi Cloud yêu cầu đọc tài nguyên: {url} [Bytes {start}-{end}]")
    try:
        headers = {'Range': f'bytes={start}-{end}'}
        async with session.get(url, headers=headers, timeout=aiohttp.ClientTimeout(total=5)) as resp:
            if resp.status in [200, 206]:
                data = await resp.read()
                if data:
                    blob = base64.b64encode(data).decode('ascii')
                else:
                    blob = base64.b64encode(b"[DONE]").decode('ascii')
            else:
                blob = base64.b64encode(b"[DONE]").decode('ascii')
    except Exception as e:
        logger.warning(f"Lỗi khi đọc resource chunk: {e}")
        blob = base64.b64encode(b"[DONE]").decode('ascii')

    return {
        "contents": [
            {
                "uri": uri or "resource://read_from_http",
                "mimeType": "audio/ogg",
                "blob": blob
            }
        ]
    }

async def run_server():
    # Khởi tạo các provider
    yt_provider = YouTubeProvider()
    zing_provider = ZingMP3Provider(yt_provider)
    stream_proxy = StreamProxy(port=STREAM_PORT)
    mcp_handler = MCPHandler(stream_proxy, yt_provider, zing_provider)

    # Khởi động HTTP Stream Proxy
    await stream_proxy.start()

    logger.info("=" * 65)
    logger.info("   XIAOZHI ROBOT MUSIC MCP SERVER (YOUTUBE & ZING MP3)   ")
    logger.info("=" * 65)
    logger.info(f"[*] Địa chỉ LAN IP máy tính  : {LAN_IP}")
    logger.info(f"[*] Cổng Stream Audio Robot   : {STREAM_PORT}")
    logger.info(f"[*] URL Luồng Ogg Opus Robot  : {STREAM_URL}")
    logger.info(f"[*] Trình giải mã FFmpeg       : Lệnh '{FFMPEG_CMD}' (qua System PATH)")
    logger.info(f"[*] Kết nối WebSocket Cloud  : {MCP_ENDPOINT[:40]}...")
    logger.info("=" * 65)

    backoff = 2
    async with aiohttp.ClientSession() as http_session:
        while True:
            try:
                logger.info("Đang kết nối tới Xiaozhi Cloud MCP Endpoint...")
                async with websockets.connect(
                    MCP_ENDPOINT,
                    open_timeout=15,
                    ping_interval=20,
                    ping_timeout=15,
                    max_size=10 * 1024 * 1024
                ) as ws:
                    logger.info("[✓] ĐÃ KẾT NỐI THÀNH CÔNG TỚI XIAOZHI CLOUD MCP ENDPOINT!")
                    backoff = 2

                    async for raw_message in ws:
                        try:
                            data = json.loads(raw_message)
                        except Exception as e:
                            logger.warning(f"Không thể giải mã JSON từ Cloud: {e} -> Data: {raw_message}")
                            continue

                        msg_id = data.get("id")
                        method = data.get("method")
                        params = data.get("params", {})

                        # 1. Initialize Handshake
                        if method == "initialize":
                            logger.info(f"Nhận yêu cầu 'initialize' từ Xiaozhi Cloud (id={msg_id})")
                            resp = {
                                "jsonrpc": "2.0",
                                "id": msg_id,
                                "result": {
                                    "protocolVersion": "2024-11-05",
                                    "capabilities": {
                                        "tools": {},
                                        "resources": {}
                                    },
                                    "serverInfo": mcp_handler.get_server_info()
                                }
                            }
                            await ws.send(json.dumps(resp))
                            logger.info("-> Đã gửi phản hồi 'initialize' thành công.")

                        # 2. Ping
                        elif method == "ping":
                            resp = {"jsonrpc": "2.0", "id": msg_id, "result": {}}
                            await ws.send(json.dumps(resp))

                        # 3. Notification: initialized
                        elif method == "notifications/initialized":
                            logger.info("[✓] Phiên MCP đã được Xiaozhi Cloud xác nhận khởi tạo hoàn tất. Sẵn sàng phục vụ robot!")

                        # 4. Tools List
                        elif method == "tools/list":
                            tools = mcp_handler.get_tools_list()
                            logger.info(f"Xiaozhi Cloud yêu cầu danh sách công cụ ('tools/list'). Đang khai báo {len(tools)} tools...")
                            resp = {
                                "jsonrpc": "2.0",
                                "id": msg_id,
                                "result": {
                                    "tools": tools
                                }
                            }
                            await ws.send(json.dumps(resp))
                            logger.info("-> Đã gửi danh sách 5 tools thành công.")

                        # 5. Tools Call
                        elif method == "tools/call":
                            tool_name = params.get("name")
                            tool_args = params.get("arguments", {})
                            logger.info(f"[!] ROBOT YÊU CẦU TOOL: '{tool_name}' - Tham số: {tool_args}")

                            tool_result = await mcp_handler.handle_call_tool(tool_name, tool_args)

                            resp = {
                                "jsonrpc": "2.0",
                                "id": msg_id,
                                "result": tool_result
                            }
                            await ws.send(json.dumps(resp))
                            logger.info(f"-> Đã gửi kết quả thực thi tool '{tool_name}' về Xiaozhi Cloud.")

                        # 6. Resources Read (Chuẩn streaming audio của Xiaozhi Cloud)
                        elif method == "resources/read":
                            res = await handle_resource_read(params, http_session)
                            resp = {
                                "jsonrpc": "2.0",
                                "id": msg_id,
                                "result": res
                            }
                            await ws.send(json.dumps(resp))

                        else:
                            if msg_id is not None:
                                resp = {
                                    "jsonrpc": "2.0",
                                    "id": msg_id,
                                    "error": {
                                        "code": -32601,
                                        "message": f"Method '{method}' not found"
                                    }
                                }
                                await ws.send(json.dumps(resp))

            except websockets.exceptions.ConnectionClosed as e:
                logger.warning(f"Mất kết nối tới Xiaozhi Cloud (Mã: {e.code}, Lý do: {e.reason}). Sẽ thử kết nối lại sau {backoff}s...")
            except Exception as e:
                logger.error(f"Lỗi kết nối WebSocket: {e}. Sẽ thử kết nối lại sau {backoff}s...")

            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)

if __name__ == "__main__":
    try:
        asyncio.run(run_server())
    except KeyboardInterrupt:
        logger.info("\nĐang dừng Xiaozhi Music MCP Server...")
