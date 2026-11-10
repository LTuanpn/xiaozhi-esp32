import sys
import asyncio
import json
import base64
import aiohttp
import websockets
from config import MCP_ENDPOINT, LAN_IP, STREAM_PORT, STREAM_URL, FFMPEG_CMD
from youtube_provider import YouTubeProvider
from zingmp3_provider import ZingMP3Provider
from stream_proxy import StreamProxy
from mcp_handler import MCPHandler
from server import handle_resource_read

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if sys.stderr and hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

async def test_youtube(yt: YouTubeProvider):
    print("\n--- [TEST 1] Kiểm tra tìm kiếm và trích xuất YouTube ---", flush=True)
    query = "Nắng ấm xa dần Sơn Tùng"
    res = yt.search_and_extract(query)
    assert res is not None, "Không tìm thấy kết quả YouTube!"
    assert res.get('audio_url'), "Không có direct audio URL!"
    assert res.get('user_agent'), "Không có user_agent!"
    print(f"[PASS] YouTube: '{res['title']}' | Ca sĩ: '{res['artist']}' | Thời lượng: {res['duration_str']}", flush=True)
    print(f"       User-Agent: {res['user_agent'][:50]}...", flush=True)
    return res

async def test_zingmp3(zing: ZingMP3Provider):
    print("\n--- [TEST 2] Kiểm tra tìm kiếm Zing MP3 + Fallback ---", flush=True)
    query = "See Tình Hoàng Thùy Linh"
    res = zing.search_and_extract(query)
    assert res is not None, "Không tìm thấy kết quả Zing MP3 / Fallback!"
    assert res.get('audio_url'), "Không có audio URL!"
    note = f" ({res.get('note')})" if res.get('note') else ""
    print(f"[PASS] Zing MP3: '{res['title']}' | Nguồn: {res['source']}{note}", flush=True)
    return res

TEST_PORT = 8766
TEST_STREAM_URL = f"http://{LAN_IP}:{TEST_PORT}/stream.ogg"

async def test_stream_proxy(proxy: StreamProxy, track: dict):
    print("\n--- [TEST 3] Kiểm tra HTTP Audio Stream Proxy (FFmpeg -> Ogg Opus) ---", flush=True)
    print(f"       FFmpeg Command: {FFMPEG_CMD} (System PATH)", flush=True)
    await proxy.start()
    proxy.set_track(track)
    print(f"[PASS] Stream Proxy đã chạy tại {TEST_STREAM_URL}", flush=True)

    async with aiohttp.ClientSession() as session:
        # Test status
        async with session.get(f"http://127.0.0.1:{TEST_PORT}/status") as resp:
            assert resp.status == 200
            status = await resp.json()
            assert status.get('status') == 'ready', f"Trạng thái không đúng: {status}"
            print(f"[PASS] Trạng thái Track: {status['title']} - {status['status']}", flush=True)

        # Test actual live stream Ogg Opus generation
        print("       Đang kiểm tra luồng phát thực tế stream.ogg...", flush=True)
        async with session.get(f"http://127.0.0.1:{TEST_PORT}/stream.ogg") as stream_resp:
            assert stream_resp.status == 200
            assert stream_resp.headers.get('Content-Type') == 'audio/ogg'
            first_chunk = await stream_resp.content.read(1024)
            assert len(first_chunk) > 0, "Không nhận được dữ liệu stream!"
            assert first_chunk[:4] == b'OggS', f"Header không đúng: {first_chunk[:4]}"
            print(f"[PASS] Nhận được dữ liệu Ogg Opus thực tế: {len(first_chunk)} bytes, Header={first_chunk[:4]}", flush=True)

        # Test resources/read handler
        print("       Đang kiểm tra handler resources/read cho Xiaozhi Cloud...", flush=True)
        read_res = await handle_resource_read({"uri": f"resource://read_from_http?url={TEST_STREAM_URL}&start=0&end=2048"}, session)
        contents = read_res.get("contents", [])
        assert len(contents) > 0
        blob = contents[0].get("blob")
        assert blob and len(blob) > 0
        decoded = base64.b64decode(blob)
        print(f"[PASS] resources/read Base64 chunk: {len(decoded)} bytes", flush=True)

    await proxy.stop()

async def test_websocket_cloud():
    print("\n--- [TEST 4] Kiểm tra kết nối WebSocket tới Xiaozhi Cloud Endpoint ---", flush=True)
    print(f"Kết nối tới: {MCP_ENDPOINT[:40]}...", flush=True)
    try:
        async with websockets.connect(MCP_ENDPOINT, open_timeout=10) as ws:
            print("[PASS] Kết nối WebSocket TCP/TLS thành công!", flush=True)
            ping_req = {"jsonrpc": "2.0", "id": 999, "method": "ping"}
            await ws.send(json.dumps(ping_req))
            print("       Đã gửi JSON-RPC ping thành công...", flush=True)
    except Exception as e:
        print(f"[FAIL] Lỗi kết nối WebSocket: {e}", flush=True)
        raise

async def main():
    print("==================================================================", flush=True)
    print("   BẮT ĐẦU KIỂM THỬ NÂNG CẤP MCP MUSIC SERVER (PYTHON 3.13)   ", flush=True)
    print("==================================================================", flush=True)

    yt = YouTubeProvider()
    zing = ZingMP3Provider(yt)
    proxy = StreamProxy(port=TEST_PORT)

    # 1. Test YouTube
    yt_res = await test_youtube(yt)

    # 2. Test Zing MP3
    await test_zingmp3(zing)

    # 3. Test Stream Proxy (Full live transcode + resources/read)
    await test_stream_proxy(proxy, yt_res)

    # 4. Test WebSocket Cloud Endpoint
    await test_websocket_cloud()

    print("\n==================================================================", flush=True)
    print("   [TẤT CẢ CÁC BÀI TEST ĐÃ VƯỢT QUA 100% THÀNH CÔNG!]   ", flush=True)
    print("==================================================================", flush=True)

if __name__ == "__main__":
    asyncio.run(main())
