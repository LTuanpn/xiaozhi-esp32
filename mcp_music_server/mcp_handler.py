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
        self.url_user_agents = {}

    def get_server_info(self):
        return {
            "name": "XiaozhiMusicMCPServer",
            "version": "1.4.0"
        }

    def get_user_agent_for_url(self, url: str) -> str:
        """Lấy User-Agent tương ứng cho URL âm thanh (đặc biệt cho YouTube CDN)."""
        if not url:
            return ""
        if url in self.url_user_agents:
            return self.url_user_agents[url]
        for cached_url, ua in self.url_user_agents.items():
            if url[:60] in cached_url or cached_url[:60] in url:
                return ua
        return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36"

    def get_tools_list(self):
        """Khai báo danh sách các công cụ hỗ trợ cho Robot Wall-E."""
        return [
            {
                "name": "play_song",
                "description": (
                    "Phát bài hát theo yêu cầu của người dùng từ YouTube hoặc Zing MP3. "
                    "Khi cần dùng tool này, hãy gọi tool NGAY LẬP TỨC mà KHÔNG nói lời chào hay câu trả lời trước khi gọi tool."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "query": {
                            "type": "string",
                            "description": "Tên bài hát, ca sĩ hoặc từ khóa bài hát người dùng muốn nghe"
                        },
                        "source": {
                            "type": "string",
                            "description": "Nguồn nhạc: 'all' (mặc định), 'youtube', hoặc 'zing'",
                            "enum": ["all", "youtube", "zing"]
                        }
                    },
                    "required": ["query"]
                }
            },
            {
                "name": "play_youtube_music",
                "description": (
                    "Tìm kiếm và phát bài hát từ YouTube cho robot Wall-E. "
                    "Khi cần dùng tool này, hãy gọi tool NGAY LẬP TỨC mà KHÔNG nói lời chào hay câu trả lời trước khi gọi tool."
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
                    "Tìm kiếm và phát bài hát từ Zing MP3 cho robot Wall-E (tự động chuyển YouTube nếu VIP). "
                    "Khi cần dùng tool này, hãy gọi tool NGAY LẬP TỨC mà KHÔNG nói lời chào hay câu trả lời trước khi gọi tool."
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

    def _build_play_response(self, info: dict, source_label: str):
        """Trả về STREAM_URL và chỉ thị bắt buộc LLM gọi self.audio.play_url trên ESP32."""
        self.stream_proxy.set_track(info)

        instruction_text = (
            f"[HỆ THỐNG: Đã chuẩn bị xong luồng nhạc '{info['title']} - {info['artist']}' tại {STREAM_URL}. "
            f"BẮT BUỘC: Bạn phải gọi ngay lập tức tool 'self.audio.play_url' với tham số url='{STREAM_URL}' để phát nhạc ra loa phần cứng. "
            f"LƯU Ý: Không nói lời chào trước khi gọi tool và không lặp lại câu đã nói trước đó!]"
        )

        return {
            "content": [
                {
                    "type": "text",
                    "text": instruction_text
                }
            ]
        }

    async def handle_call_tool(self, name: str, arguments: dict):
        """Thực thi tool tương ứng khi có request từ Xiaozhi Cloud."""
        logger.info(f"Đang thực thi Tool: '{name}' với tham số: {arguments}")

        if name in ["play_song", "play_youtube_music", "play_zing_mp3"]:
            query = arguments.get("query", "").strip()
            source = arguments.get("source", "all").lower() if name == "play_song" else ("youtube" if name == "play_youtube_music" else "zing")

            if not query:
                return {
                    "isError": True,
                    "content": [{"type": "text", "text": "Bạn muốn nghe bài hát gì? Hãy nói tên bài hát cho mình nhé!"}]
                }

            info = None
            source_label = "YouTube"

            if source == "zing":
                info = self.zing_provider.search_and_extract(query)
                source_label = info.get('source', 'Zing MP3') if info else 'Zing MP3'
            else:
                # Mặc định ưu tiên YouTube hoặc source == 'youtube' / 'all'
                info = self.yt_provider.search_and_extract(query)
                if not info and source == "all":
                    info = self.zing_provider.search_and_extract(query)
                    source_label = info.get('source', 'Zing MP3') if info else 'Zing MP3'

            if not info or not info.get("audio_url"):
                return {
                    "isError": True,
                    "content": [{"type": "text", "text": f"Mình không tìm thấy bài hát '{query}' rồi."}]
                }

            return self._build_play_response(info, source_label)

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
            instruction_text = (
                "[HỆ THỐNG: Đã dừng luồng stream trên máy tính. BẮT BUỘC: Hãy gọi tiếp tool "
                "'self.audio.stop_playback' trên robot để dừng âm thanh ngay lập tức. "
                "Tuyệt đối không lặp lại câu đã nói trước đó!]"
            )
            return {"content": [{"type": "text", "text": instruction_text}]}

        elif name == "get_player_status":
            track = self.stream_proxy.current_track
            if track.get("status") == "playing":
                msg = f"Đang phát bài: {track.get('title')} của ca sĩ {track.get('artist')} ({track.get('duration_str')}) từ {track.get('source', 'YouTube')}."
            elif track.get("status") == "ready":
                msg = f"Đã sẵn sàng bài {track.get('title')} của ca sĩ {track.get('artist')}."
            else:
                msg = "Hiện tại robot đang không phát bài hát nào."
            return {"content": [{"type": "text", "text": msg}]}

        else:
            return {"isError": True, "content": [{"type": "text", "text": f"Lệnh '{name}' không được hỗ trợ."}]}
