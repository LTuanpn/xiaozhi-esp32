import sys
import logging
import asyncio
from aiohttp import web
from config import STREAM_PORT, AUDIO_SAMPLE_RATE, AUDIO_BITRATE, AUDIO_CHANNELS, FFMPEG_CMD

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if sys.stderr and hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

logger = logging.getLogger("StreamProxy")

class StreamProxy:
    def __init__(self, port: int = STREAM_PORT):
        self.port = port
        self.app = web.Application()
        self.runner = None
        self.site = None
        self.current_track = {
            'title': 'Chưa phát',
            'artist': '',
            'audio_url': None,
            'duration_str': '00:00',
            'user_agent': None,
            'status': 'idle'
        }
        self.active_process = None
        self.app.router.add_get('/stream.ogg', self.handle_stream)
        self.app.router.add_get('/status', self.handle_status)
        self.app.router.add_get('/stop', self.handle_stop)

    def set_track(self, track_info: dict):
        """Cập nhật bài hát hiện tại và dừng bài hát cũ nếu đang chạy."""
        self.stop_current()
        self.current_track = {
            'title': track_info.get('title', 'Unknown'),
            'artist': track_info.get('artist', ''),
            'audio_url': track_info.get('audio_url'),
            'duration_str': track_info.get('duration_str', '00:00'),
            'source': track_info.get('source', 'Unknown'),
            'user_agent': track_info.get('user_agent'),
            'status': 'ready'
        }
        logger.info(f"Đã nạp bài hát vào Stream Proxy: {self.current_track['title']} - {self.current_track['artist']}")

    def stop_current(self):
        """Hủy tiến trình transcode hiện tại."""
        if self.active_process:
            try:
                logger.info("Đang dừng tiến trình phát nhạc...")
                self.active_process.terminate()
            except Exception as e:
                logger.warning(f"Lỗi khi dừng tiến trình: {e}")
            self.active_process = None
        self.current_track['status'] = 'stopped'

    async def handle_stream(self, request):
        """Xử lý yêu cầu stream audio từ Robot ESP32 (Mono Ogg Opus)."""
        audio_url = self.current_track.get('audio_url')
        if not audio_url:
            return web.Response(text="Không có bài hát nào sẵn sàng để phát", status=404)

        ua = self.current_track.get('user_agent') or 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36'
        logger.info(f"Robot ESP32 đã kết nối để nhận luồng Ogg Opus: {self.current_track['title']}")
        self.stop_current()

        response = web.StreamResponse(
            status=200,
            reason='OK',
            headers={
                'Content-Type': 'audio/ogg',
                'Cache-Control': 'no-cache, no-store',
                'Connection': 'close',
                'Accept-Ranges': 'none'
            }
        )
        await response.prepare(request)

        # Lệnh FFmpeg transcode thời gian thực sang Mono Ogg Opus (kèm user_agent chống 403)
        cmd = [
            FFMPEG_CMD,
            '-y',
            '-user_agent', ua,
            '-reconnect', '1',
            '-reconnect_streamed', '1',
            '-reconnect_delay_max', '5',
            '-i', audio_url,
            '-vn',
            '-c:a', 'libopus',
            '-b:a', AUDIO_BITRATE,
            '-ar', str(AUDIO_SAMPLE_RATE),
            '-ac', str(AUDIO_CHANNELS),
            '-application', 'audio',
            '-frame_duration', '20',
            '-page_duration', '20000',
            '-flush_packets', '1',
            '-f', 'ogg',
            'pipe:1'
        ]

        try:
            self.active_process = await asyncio.create_subprocess_exec(
                *cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL
            )
            self.current_track['status'] = 'playing'

            while True:
                chunk = await self.active_process.stdout.read(4096)
                if not chunk:
                    break
                await response.write(chunk)

            await self.active_process.wait()
        except asyncio.CancelledError:
            logger.info("ESP32 đã ngắt kết nối stream.")
        except Exception as e:
            logger.error(f"Lỗi trong quá trình stream: {e}")
        finally:
            self.stop_current()
            self.current_track['status'] = 'idle'

        return response

    async def handle_status(self, request):
        """Trả về thông tin bài hát hiện tại dạng JSON."""
        return web.json_response(self.current_track)

    async def handle_stop(self, request):
        """Dừng bài hát theo yêu cầu HTTP."""
        self.stop_current()
        return web.json_response({'status': 'stopped', 'message': 'Đã dừng phát nhạc'})

    async def start(self):
        """Khởi động HTTP server."""
        self.runner = web.AppRunner(self.app)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, '0.0.0.0', self.port)
        await self.site.start()
        logger.info(f"HTTP Audio Stream Proxy đã chạy tại: http://0.0.0.0:{self.port}/stream.ogg (FFmpeg: {FFMPEG_CMD})")

    async def stop(self):
        """Dừng HTTP server."""
        self.stop_current()
        if self.runner:
            await self.runner.cleanup()
        logger.info("HTTP Audio Stream Proxy đã dừng.")
