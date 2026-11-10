import sys
import base64
import asyncio
import aiohttp

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

from youtube_provider import YouTubeProvider
from zingmp3_provider import ZingMP3Provider
from stream_proxy import StreamProxy
from mcp_handler import MCPHandler
from server import handle_resource_read

async def test_full_pipeline():
    print("[*] 1. Khởi tạo MCPHandler và Providers...")
    yt = YouTubeProvider()
    zing = ZingMP3Provider(yt)
    proxy = StreamProxy(port=8768)
    handler = MCPHandler(proxy, yt, zing)

    tools = handler.get_tools_list()
    tool_names = [t['name'] for t in tools]
    print(f"[*] Các công cụ được khai báo ({len(tools)}): {tool_names}")
    assert "play_song" in tool_names, "Lỗi: Không tìm thấy tool 'play_song'"

    print("\n[*] 2. Thử nghiệm gọi tool 'play_song' với query: 'Tay Thi MCK'...")
    res = await handler.handle_call_tool("play_song", {"query": "Tay Thi MCK"})
    assert not res.get("isError"), f"Lỗi gọi tool: {res}"

    contents = res.get("content", [])
    assert len(contents) >= 2, f"Lỗi nội dung trả về: {contents}"

    text_item = contents[0]
    link_item = contents[1]

    print(f"[✓] Text phản hồi cho người dùng: \"{text_item.get('text')}\"")
    assert "BẮT BUỘC" not in text_item.get('text', ''), "Lỗi: Vẫn còn chứa mệnh lệnh gây lặp lời"
    assert link_item.get("type") == "resource_link", f"Lỗi: Type phải là 'resource_link', thực tế: {link_item.get('type')}"
    assert link_item.get("uri") == "resource://read_from_http", f"Lỗi URI: {link_item.get('uri')}"
    assert link_item.get("mimeType") == "audio/mpeg", f"Lỗi mimeType: {link_item.get('mimeType')}"

    audio_url = link_item.get("description")
    print(f"[✓] Link audio chuẩn: {audio_url[:80]}...")

    print("\n[*] 3. Thử nghiệm đọc chunk 0-102400 bytes qua 'handle_resource_read'...")
    params = {
        "uri": "resource://read_from_http",
        "arguments": {
            "url": audio_url,
            "start": 0,
            "end": 102400
        }
    }

    async with aiohttp.ClientSession() as session:
        read_res = await handle_resource_read(params, session, handler)
        assert "contents" in read_res, f"Lỗi format resources/read: {read_res}"
        blob_b64 = read_res["contents"][0]["blob"]
        raw_data = base64.b64decode(blob_b64)
        print(f"[✓] Kích thước dữ liệu đọc được: {len(raw_data)} bytes")
        assert raw_data != b"[DONE]", "Lỗi: Server trả về [DONE] thay vì dữ liệu audio (nghi vấn bị 403 Forbidden)"
        assert len(raw_data) > 50000, f"Lỗi: Dữ liệu quá nhỏ ({len(raw_data)} bytes)"

    print("\n" + "=" * 60)
    print(">>> TẤT CẢ CÁC BƯỚC TEST MCP AUDIO PIPELINE THÀNH CÔNG 100%! <<<")
    print("=" * 60)

if __name__ == "__main__":
    asyncio.run(test_full_pipeline())
