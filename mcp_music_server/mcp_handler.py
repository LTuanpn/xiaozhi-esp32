import asyncio
import json
import logging
from concurrent.futures import ThreadPoolExecutor
from music_selection import AmbiguousMusic
import utility_tools

logger = logging.getLogger(__name__)

def result(data, error=False):
    return {'isError': error, 'content': [{'type': 'text', 'text': json.dumps(data, ensure_ascii=False)}]}

class MCPHandler:
    def __init__(self, stream_proxy, yt_provider, zing_provider):
        self.stream_proxy = stream_proxy
        self.yt_provider = yt_provider
        self.zing_provider = zing_provider
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix='music-search')
        self._search_future = None
        self._intent = 0

    def get_server_info(self):
        return {'name': 'XiaozhiMusicMCPServer', 'version': '2.1.0'}

    def get_tools_list(self):
        query_schema = {'type': 'object', 'properties': {
            'query': {'type': 'string', 'minLength': 1, 'maxLength': 256, 'description': 'Tên bài hoặc URL video YouTube chính xác'},
            'artist': {'type': 'string', 'maxLength': 120, 'description': 'Ca sĩ nếu người dùng nêu rõ'},
            'source': {'type': 'string', 'enum': ['all', 'youtube', 'zing']}}, 'required': ['query']}
        play_description = ('Dùng khi người dùng yêu cầu mở/phát/nghe bài hát. Không dùng cho kiến thức hay tìm web. '
            'Gọi trước khi nói. Khi status=ready, gọi self.audio.play_url với stream_url rồi kết thúc lượt, '
            'không đọc URL hoặc nói đè nhạc. Khi needs_clarification, hỏi chọn bài, không tự phát hoặc đổi chủ đề.')
        return [{'name': name, 'description': description, 'inputSchema': schema} for name, description, schema in [
            ('play_song', play_description, query_schema),
            ('play_youtube_music', 'Alias YouTube của play_song. ' + play_description, query_schema),
            ('play_zing_mp3', 'Alias Zing của play_song. ' + play_description, query_schema),
            ('search_music', 'Chỉ tìm danh sách bài hát, không phát.', query_schema),
            ('stop_music', 'Dừng nhạc. Sau khi dừng server, gọi self.audio.stop_playback trên robot; xác nhận một lần.', {'type':'object','properties':{}}),
            ('get_player_status', 'Xem bài và trạng thái phát nhạc.', {'type':'object','properties':{}})]] + [
                {key: value for key, value in tool.items() if key != "category"}
                for tool, _ in utility_tools.TOOLS.values()
            ]

    def cancel_pending(self):
        self._intent += 1

    async def _search(self, function, *args):
        # A timed-out yt-dlp thread still occupies this single slot until it really ends.
        if self._search_future is not None and not self._search_future.done():
            raise RuntimeError('search_busy')
        future = asyncio.get_running_loop().run_in_executor(self._executor, function, *args)
        self._search_future = future
        # Retrieve eventual exceptions even if the caller timed out or disconnected.
        future.add_done_callback(lambda done: done.exception() if not done.cancelled() else None)
        return await asyncio.wait_for(asyncio.shield(future), 25)

    def _build_play_response(self, info, source_label=None):
        url = self.stream_proxy.set_track(info)
        return result({'status': 'ready', 'title': info['title'], 'artist': info['artist'],
            'source': info.get('source'), 'video_id': info.get('video_id'), 'stream_url': url,
            'next_tool': 'self.audio.play_url', 'arguments': {'url': url}})

    async def handle_call_tool(self, name, arguments):
        if not isinstance(arguments, dict):
            return result({'status': 'invalid_arguments'}, True)
        if isinstance(name, str) and name in utility_tools.TOOLS:
            try:
                return result({"status": "ok", "value": utility_tools.run(name, arguments)})
            except (ValueError, TypeError, ArithmeticError, RecursionError):
                return result({"status": "invalid_arguments", "message": "Dữ liệu không hợp lệ cho công cụ này."}, True)
        if name == 'stop_music':
            self.cancel_pending()
            self.stream_proxy.stop_current()
            return result({'status': 'stopped', 'next_tool': 'self.audio.stop_playback', 'arguments': {}})
        if name == 'get_player_status':
            return result(self.stream_proxy.public_status())
        if name not in ('play_song', 'play_youtube_music', 'play_zing_mp3', 'search_music'):
            return result({'status': 'unknown_tool'}, True)
        query, artist = arguments.get('query', ''), arguments.get('artist', '')
        source = arguments.get('source', 'all')
        if not isinstance(query, str) or not 1 <= len(query.strip()) <= 256 or not isinstance(artist, str) or len(artist) > 120 or source not in ('all', 'youtube', 'zing'):
            return result({'status': 'invalid_arguments', 'message': 'Cần tên bài/ca sĩ hợp lệ.'}, True)
        query = query.strip()
        if name == 'play_youtube_music': source = 'youtube'
        if name == 'play_zing_mp3': source = 'zing'
        if self._search_future is not None and not self._search_future.done():
            return result({'status': 'busy', 'message': 'Đang tìm bài; có thể gọi stop_music để hủy.'}, True)
        self._intent += 1
        intent = self._intent
        logger.info('Music request tool=%s query=%s artist=%s source=%s', name, query, artist, source)
        try:
            if name == 'search_music':
                provider = self.zing_provider.search_suggestions if source == 'zing' else self.yt_provider.search_multiple
                rows = await self._search(provider, (query + ' ' + artist).strip())
                return result({'status': 'found', 'choices': rows[:5]})
            provider = self.zing_provider if source == 'zing' else self.yt_provider
            info = await self._search(provider.search_and_extract, query, artist)
            if intent != self._intent:
                return result({'status': 'cancelled'})
            if not info and source == 'all':
                info = await self._search(self.zing_provider.search_and_extract, query, artist)
            if intent != self._intent:
                return result({'status': 'cancelled'})
            if not info or not info.get('audio_url'):
                return result({'status': 'not_found', 'message': 'Không tìm thấy nguồn nhạc phù hợp.'}, True)
            return self._build_play_response(info)
        except AmbiguousMusic as exc:
            return result({'status': 'needs_clarification', 'choices': exc.choices})
        except asyncio.TimeoutError:
            return result({'status': 'timeout', 'message': 'Nguồn nhạc phản hồi chậm.'}, True)
        except Exception as exc:
            logger.warning('Music request failed: %s', type(exc).__name__)
            return result({'status': 'error', 'message': 'Không thể lấy nguồn nhạc; thử tên bài và ca sĩ rõ hơn.'}, True)

    def close(self):
        self.cancel_pending()
        self._executor.shutdown(wait=False, cancel_futures=True)
