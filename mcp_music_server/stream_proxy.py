"""One bounded transcoder, immutable track tickets and deterministic cleanup."""
import asyncio
import hmac
import logging
import secrets
import sys
import time
from urllib.parse import urlsplit
from aiohttp import web
from config import STREAM_PORT, STREAM_HOST_OVERRIDE, get_lan_ip, ADMIN_TOKEN, AUDIO_SAMPLE_RATE, AUDIO_BITRATE, AUDIO_CHANNELS, FFMPEG_CMD

logger = logging.getLogger(__name__)

class StreamProxy:
    def __init__(self, port=STREAM_PORT):
        self.port = port
        self.app = web.Application()
        self.app.router.add_get('/stream/{ticket}.ogg', self.handle_stream)
        self.app.router.add_get('/status', self.handle_status)
        self.app.router.add_post('/stop', self.handle_stop)
        self.runner = self.site = None
        self.current_track = {'title': '', 'artist': '', 'status': 'idle'}
        self.active_process = None
        self._generation = 0
        self._ticket = None
        self._expires = 0
        self._start_lock = asyncio.Lock()
        self._cleanup_tasks = set()

    async def _reap(self, proc):
        if proc.returncode is None:
            try:
                proc.terminate()
            except ProcessLookupError:
                pass
        try:
            await asyncio.wait_for(proc.wait(), 2)
        except asyncio.TimeoutError:
            try:
                proc.kill()
            except ProcessLookupError:
                pass
            await proc.wait()

    def stop_current(self):
        self._generation += 1
        self._ticket = None
        proc, self.active_process = self.active_process, None
        self.current_track['status'] = 'stopped'
        if proc is not None:
            task = asyncio.create_task(self._reap(proc))
            self._cleanup_tasks.add(task)
            task.add_done_callback(self._cleanup_tasks.discard)

    def set_track(self, info):
        if urlsplit(info.get('audio_url', '')).scheme not in ('https', 'http'):
            raise ValueError('Provider returned an invalid audio URL')
        self.stop_current()
        self._ticket = secrets.token_urlsafe(24)
        self._expires = time.monotonic() + 3600
        self.current_track = {**info, 'status': 'ready'}
        # The PC may move between Wi-Fi networks while the MCP websocket reconnects.
        # Refresh the automatic LAN address for each track; preserve an explicit host.
        host = STREAM_HOST_OVERRIDE or get_lan_ip()
        if ':' in host and not host.startswith('['):
            host = '[' + host + ']'
        return f'http://{host}:{self.port}/stream/{self._ticket}.ogg'

    def public_status(self):
        return {k: self.current_track.get(k) for k in
                ('title', 'artist', 'duration_str', 'source', 'status', 'video_id', 'error')}

    def _authorized(self, request):
        return bool(ADMIN_TOKEN) and hmac.compare_digest(request.headers.get('Authorization', ''), 'Bearer ' + ADMIN_TOKEN)

    async def handle_status(self, request):
        if not self._authorized(request):
            raise web.HTTPForbidden()
        return web.json_response(self.public_status())

    async def handle_stop(self, request):
        if not self._authorized(request):
            raise web.HTTPForbidden()
        self.stop_current()
        return web.json_response({'status': 'stopped'})

    async def handle_stream(self, request):
        ticket = request.match_info.get('ticket', '')
        proc = response = None
        generation = self._generation
        try:
            async with self._start_lock:
                if not self._ticket or not hmac.compare_digest(ticket, self._ticket) or time.monotonic() > self._expires:
                    raise web.HTTPGone(text='Track expired or replaced')
                if self.active_process is not None:
                    raise web.HTTPConflict(text='Track is already streaming')
                generation = self._generation
                track = dict(self.current_track)
                cmd = [FFMPEG_CMD, '-hide_banner', '-loglevel', 'error', '-nostdin',
                       '-rw_timeout', '10000000', '-user_agent', track.get('user_agent') or 'Mozilla/5.0',
                       '-re', '-i', track['audio_url'], '-vn', '-c:a', 'libopus', '-b:a', AUDIO_BITRATE,
                       '-ar', str(AUDIO_SAMPLE_RATE), '-ac', str(AUDIO_CHANNELS), '-application', 'audio',
                       '-frame_duration', '20', '-page_duration', '20000', '-flush_packets', '1', '-f', 'ogg', 'pipe:1']
                options = {'creationflags': 0x08000000} if sys.platform == 'win32' else {}
                proc = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE,
                            stderr=asyncio.subprocess.DEVNULL, **options)
                if generation != self._generation:
                    raise web.HTTPGone(text='Track replaced during startup')
                self.active_process = proc
                first = await asyncio.wait_for(proc.stdout.read(4096), 10)
                if generation != self._generation:
                    raise web.HTTPGone(text='Track replaced during startup')
                if not first.startswith(b'OggS'):
                    raise RuntimeError('FFmpeg produced no Ogg stream')
                response = web.StreamResponse(headers={'Content-Type': 'audio/ogg',
                    'Cache-Control': 'no-store', 'Accept-Ranges': 'none', 'Connection': 'close'})
                await response.prepare(request)
                if generation != self._generation:
                    return response
                self.current_track['status'] = 'playing'
            await response.write(first)
            while generation == self._generation:
                chunk = await asyncio.wait_for(proc.stdout.read(4096), 15)
                if not chunk or generation != self._generation:
                    break
                await asyncio.wait_for(response.write(chunk), 10)
            code = await asyncio.wait_for(proc.wait(), 2)
            if generation == self._generation and code != 0:
                raise RuntimeError(f'FFmpeg exited with code {code}')
            return response
        except web.HTTPException:
            raise
        except asyncio.CancelledError:
            raise
        except (ConnectionResetError, BrokenPipeError):
            return response
        except Exception as exc:
            logger.warning('Stream failed: %s', type(exc).__name__)
            if generation == self._generation:
                self.current_track.update(status='error', error=type(exc).__name__)
            if response is None:
                raise web.HTTPBadGateway(text='Audio source could not be opened') from None
            return response
        finally:
            if proc is not None:
                await self._reap(proc)
            if proc is not None and self.active_process is proc:
                self.active_process = None
                if generation == self._generation:
                    self._ticket = None
                    if self.current_track['status'] != 'error':
                        self.current_track['status'] = 'idle'

    async def start(self):
        self.runner = web.AppRunner(self.app)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, '0.0.0.0', self.port)
        await self.site.start()
        logger.info('Audio proxy listening on port %s', self.port)

    async def stop(self):
        self.stop_current()
        if self.runner:
            await self.runner.cleanup()
        if self._cleanup_tasks:
            await asyncio.gather(*list(self._cleanup_tasks), return_exceptions=True)
