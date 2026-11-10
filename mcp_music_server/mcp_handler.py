import sys
import logging
from config import STREAM_URL
from youtube_provider import YouTubeProvider
from zingmp3_provider import ZingMP3Provider
from stream_proxy import StreamProxy

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if sys.stderr and hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

logger = logging.getLogger("MCPHandler")

class MCPHandler:
    def __init__(self, stream_proxy: StreamProxy, yt_provider: YouTubeProvider, zing_provider: ZingMP3Provider):
        self.stream_proxy = stream_proxy
        self.yt_provider = yt_provider
        self.zing_provider = zing_provider

    def get_server_info(self):
        return {
            "name": "XiaozhiMusicMCPServer",
            "version": "1.2.0"
        }

    def get_tools_list(self):
        """Khai báo danh sách các công cụ hỗ trợ cho Robot Wall-E."""
        return [
            {
                "name": "play_youtube_music",
                "description": (
                    "Tìm kiếm bài hát trên YouTube và tạo luồng phát cho robot Wall-E (ESP32). "
                    "Hãy gọi tool này khi người dùng yêu cầu bật nhạc, nghe nhạc từ YouTube. "
                    "Sau khi gọi tool này, BẮT BUỘC bạn phải gọi tiếp tool self.audio.play_url(url=...) trên robot để phát ra loa."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "query": {
                            "type": "string",
                            "description": "Tên bài hát, ca sĩ hoặc từ khóa tìm kiếm trên YouTube"
                        }
                    },
                    "required": ["query"]
                }
            },
            {
                "name": "play_zing_mp3",
                "description": (
                    "Tìm kiếm bài hát trên Zing MP3 và tạo luồng phát cho robot Wall-E (ESP32). "
                    "Tự động chuyển bài hát chất lượng cao từ YouTube nếu bài trên Zing MP3 bị khóa bản quyền VIP. "
                    "Sau khi gọi tool này, BẮT BUỘC bạn phải gọi tiếp tool self.audio.play_url(url=...) trên robot để phát ra loa."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "query": {
                            "type": "string",
                            "description": "Tên bài hát hoặc ca sĩ trên Zing MP3"
                        }
                    },
                    "required": ["query"]
                }
            },
            {
                "name": "search_music",
                "description": (
                    "Tìm kiếm và tra cứu danh sách bài hát từ YouTube hoặc Zing MP3 kèm thời lượng, ca sĩ "
                    "để thông báo cho người dùng mà không phát nhạc ngay."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "query": {
                            "type": "string",
                            "description": "Tên bài hát cần tra cứu"
                        },
                        "source": {
                            "type": "string",
                            "description": "Nguồn tìm kiếm: 'youtube', 'zing' hoặc 'all'",
                            "enum": ["youtube", "zing", "all"]
                        }
                    },
                    "required": ["query"]
                }
            },
            {
                "name": "stop_music",
                "description": "Dừng phát nhạc ngay lập tức trên robot Wall-E.",
                "inputSchema": {
                    "type": "object",
                    "properties": {}
                }
            },
            {
                "name": "get_player_status",
                "description": "Kiểm tra thông tin bài hát đang phát hoặc trạng thái trình phát nhạc của robot.",
                "inputSchema": {
                    "type": "object",
                    "properties": {}
                }
            }
        ]

    async def handle_call_tool(self, name: str, arguments: dict):
        """Thực thi tool tương ứng khi có request từ Xiaozhi Cloud."""
        logger.info(f"Đang thực thi Tool: '{name}' với tham số: {arguments}")

        if name == "play_youtube_music":
            query = arguments.get("query", "").strip()
            if not query:
                return {
                    "isError": True,
                    "content": [{"type": "text", "text": "Bạn muốn nghe bài gì trên YouTube? Hãy nói tên bài hát cho mình nhé!"}]
                }

            info = self.yt_provider.search_and_extract(query)
            if not info or not info.get("audio_url"):
                return {
                    "isError": True,
                    "content": [{"type": "text", "text": f"Mình không tìm thấy bài hát '{query}' trên YouTube rồi."}]
                }

            # Nạp bài hát vào Stream Proxy
            self.stream_proxy.set_track(info)

            # Chỉ thị bắt buộc để AI Cloud gọi tiếp tool phần cứng trên robot
            speech_text = (
                f"Đã tìm thấy bài hát \"{info['title']}\" của {info['artist']}. "
                f"BẮT BUỘC: Hãy gọi ngay công cụ self.audio.play_url(url='{STREAM_URL}') "
                f"để robot Wall-E phát bài hát này ra loa."
            )
            
            return {
                "content": [
                    {"type": "text", "text": speech_text},
                    {
                        "type": "resource",
                        "resource": {
                            "uri": "resource://read_from_http",
                            "mimeType": "audio/ogg",
                            "text": STREAM_URL
                        }
                    }
                ],
                "audio_url": STREAM_URL,
                "track": {
                    "title": info['title'],
                    "artist": info['artist'],
                    "duration": info['duration_str'],
                    "source": "YouTube",
                    "stream_url": STREAM_URL
                }
            }

        elif name == "play_zing_mp3":
            query = arguments.get("query", "").strip()
            if not query:
                return {
                    "isError": True,
                    "content": [{"type": "text", "text": "Bạn muốn nghe bài hát nào trên Zing MP3?"}]
                }

            info = self.zing_provider.search_and_extract(query)
            if not info or not info.get("audio_url"):
                return {
                    "isError": True,
                    "content": [{"type": "text", "text": f"Mình không tìm thấy bài hát '{query}' trên Zing MP3 rồi."}]
                }

            # Nạp bài hát vào Stream Proxy
            self.stream_proxy.set_track(info)

            speech_text = (
                f"Đã tìm thấy bài hát \"{info['title']}\" của {info['artist']}. "
                f"BẮT BUỘC: Hãy gọi ngay công cụ self.audio.play_url(url='{STREAM_URL}') "
                f"để robot Wall-E phát bài hát này ra loa."
            )

            return {
                "content": [
                    {"type": "text", "text": speech_text},
                    {
                        "type": "resource",
                        "resource": {
                            "uri": "resource://read_from_http",
                            "mimeType": "audio/ogg",
                            "text": STREAM_URL
                        }
                    }
                ],
                "audio_url": STREAM_URL,
                "track": {
                    "title": info['title'],
                    "artist": info['artist'],
                    "duration": info['duration_str'],
                    "source": info.get('source', 'Zing MP3'),
                    "stream_url": STREAM_URL
                }
            }

        elif name == "search_music":
            query = arguments.get("query", "").strip()
            source = arguments.get("source", "all").lower()
            results = []

            if source in ["youtube", "all"]:
                yt_results = self.yt_provider.search_multiple(query, limit=3)
                results.extend(yt_results)

            if source in ["zing", "all"]:
                zing_results = self.zing_provider.search_suggestions(query)
                for z in zing_results[:3]:
                    mins, secs = divmod(z.get('duration', 0), 60)
                    results.append({
                        'source': 'Zing MP3',
                        'title': z.get('title'),
                        'artist': z.get('artist'),
                        'duration_str': f"{mins:02d}:{secs:02d}",
                        'url': z.get('link')
                    })

            if not results:
                return {"content": [{"type": "text", "text": f"Mình không tìm thấy bài hát nào cho từ khóa '{query}'."}]}

            lines = [f"Dưới đây là một số bài hát tìm thấy cho '{query}':"]
            for idx, r in enumerate(results, 1):
                lines.append(f"{idx}. {r['title']} của {r['artist']} ({r['duration_str']}) trên {r['source']}")

            return {"content": [{"type": "text", "text": "\n".join(lines)}]}

        elif name == "stop_music":
            self.stream_proxy.stop_current()
            msg = "Đã dừng phát nhạc trên máy tính. BẮT BUỘC: Hãy gọi thêm công cụ self.audio.stop_playback() trên robot để dừng âm thanh ngay lập tức."
            return {"content": [{"type": "text", "text": msg}]}

        elif name == "get_player_status":
            track = self.stream_proxy.current_track
            if track.get("status") == "playing":
                msg = f"Đang phát bài: {track.get('title')} của ca sĩ {track.get('artist')} ({track.get('duration_str')}) từ {track.get('source', 'YouTube')}."
            elif track.get("status") == "ready":
                msg = f"Đã sẵn sàng phát bài {track.get('title')} của ca sĩ {track.get('artist')}, đang chờ robot kết nối tải luồng âm thanh."
            else:
                msg = "Hiện tại robot đang không phát bài hát nào."
            return {"content": [{"type": "text", "text": msg}]}

        else:
            return {"isError": True, "content": [{"type": "text", "text": f"Lệnh '{name}' không được hỗ trợ."}]}
