import asyncio
import unittest
from unittest.mock import AsyncMock, Mock, patch
from aiohttp import web
from stream_proxy import StreamProxy

class ProxyTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.proxy = StreamProxy()
        self.track = {'title':'Test','artist':'Artist','audio_url':'https://example.com/audio'}
    async def asyncTearDown(self): await self.proxy.stop()

    async def test_replaced_and_invalid_tickets_do_not_destroy_current_track(self):
        first = self.proxy.set_track(self.track)
        second = self.proxy.set_track(self.track)
        ticket = self.proxy._ticket
        self.assertNotEqual(first, second)
        request = Mock(match_info={'ticket':'invalid'})
        with self.assertRaises(web.HTTPGone): await self.proxy.handle_stream(request)
        self.assertEqual(self.proxy._ticket, ticket)
        self.assertEqual(self.proxy.current_track['status'], 'ready')

    async def test_status_does_not_expose_upstream_or_ticket(self):
        self.proxy.set_track(self.track)
        self.assertNotIn('audio_url', self.proxy.public_status())
        self.assertNotIn('ticket', self.proxy.public_status())

    async def test_stream_url_tracks_network_changes_and_actual_port(self):
        self.proxy.port = 9876
        with patch('stream_proxy.STREAM_HOST_OVERRIDE', ''), patch('stream_proxy.get_lan_ip',
                side_effect=['192.168.1.10', '192.168.198.10']):
            first = self.proxy.set_track(self.track)
            second = self.proxy.set_track(self.track)
        self.assertTrue(first.startswith('http://192.168.1.10:9876/stream/'))
        self.assertTrue(second.startswith('http://192.168.198.10:9876/stream/'))

    async def test_explicit_stream_host_is_preserved(self):
        with patch('stream_proxy.STREAM_HOST_OVERRIDE', 'music.example.test'), patch('stream_proxy.get_lan_ip') as lookup:
            url = self.proxy.set_track(self.track)
            lookup.assert_not_called()
        self.assertTrue(url.startswith('http://music.example.test:'))

    async def test_admin_is_denied_without_credentials(self):
        request = Mock(headers={})
        with self.assertRaises(web.HTTPForbidden): await self.proxy.handle_stop(request)
        with self.assertRaises(web.HTTPForbidden): await self.proxy.handle_status(request)

    async def test_ffmpeg_failure_returns_error_before_http_success(self):
        self.proxy.set_track(self.track)
        proc = Mock(returncode=1)
        proc.stdout.read = AsyncMock(return_value=b'')
        proc.wait = AsyncMock(return_value=1)
        with patch('stream_proxy.asyncio.create_subprocess_exec', AsyncMock(return_value=proc)):
            with self.assertRaises(web.HTTPBadGateway):
                await self.proxy.handle_stream(Mock(match_info={'ticket':self.proxy._ticket}))
        self.assertEqual(self.proxy.current_track['status'], 'error')
        self.assertIsNone(self.proxy.active_process)

    async def test_stop_while_subprocess_starts_reaps_orphan(self):
        self.proxy.set_track(self.track)
        proc = Mock(returncode=None)
        proc.wait = AsyncMock(return_value=0)
        async def spawn(*args, **kwargs):
            self.proxy.stop_current()
            return proc
        with patch('stream_proxy.asyncio.create_subprocess_exec', spawn):
            with self.assertRaises(web.HTTPGone):
                await self.proxy.handle_stream(Mock(match_info={'ticket':self.proxy._ticket}))
        proc.terminate.assert_called_once()
        self.assertIsNone(self.proxy.active_process)

if __name__ == '__main__': unittest.main()
