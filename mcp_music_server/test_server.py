import asyncio
import json
import threading
import unittest
from unittest.mock import Mock
from music_selection import choose_candidate
from youtube_provider import YouTubeProvider
from mcp_handler import MCPHandler
from server import dispatch, serve_connection
from stream_proxy import StreamProxy

def payload(reply):
    return json.loads(reply['content'][0]['text'])

class SelectionTests(unittest.TestCase):
    def test_correct_music_beats_unrelated_first_result(self):
        rows = [{'title':'Kiến thức thú vị', 'artist':'Fun Facts', 'duration':300},
                {'title':'SON TUNG M-TP x TYGA | COME MY WAY | OFFICIAL MUSIC VIDEO', 'artist':'Sơn Tùng M-TP Official', 'duration':235},
                {'title':'Come My Way softer version', 'artist':'Sơn Tùng M-TP Official', 'duration':253}]
        selected, _ = choose_candidate('come my way', 'Sơn Tùng MTP', rows)
        self.assertIn('OFFICIAL MUSIC VIDEO', selected['title'])

    def test_ambiguous_artist_does_not_autoplay(self):
        selected, choices = choose_candidate('Hello', '', [
            {'title':'Hello', 'artist':'Adele Official'}, {'title':'Hello','artist':'Lionel Richie Official'}])
        self.assertIsNone(selected)
        self.assertEqual(len(choices), 2)

    def test_no_match_does_not_autoplay(self):
        selected, _ = choose_candidate('come my way', 'Son Tung', [{'title':'Kien thuc thu vi','artist':'News'}])
        self.assertIsNone(selected)

    def test_explicit_remix_is_allowed(self):
        selected, _ = choose_candidate('Hello remix', 'Adele', [{'title':'Adele Hello remix','artist':'Adele Official'}])
        self.assertIsNotNone(selected)

    def test_url_allowlist_and_video_id(self):
        for url in ('http://127.0.0.1/admin', 'https://youtube.com.evil.test/watch?v=SlQR9iu09bQ', 'https://www.youtube.com/playlist?list=x'):
            with self.assertRaises(ValueError): YouTubeProvider.video_url(url)
        self.assertEqual(YouTubeProvider.video_url('https://youtu.be/SlQR9iu09bQ'), 'https://www.youtube.com/watch?v=SlQR9iu09bQ')

class FakeWebSocket:
    def __init__(self):
        self.incoming = asyncio.Queue()
        self.sent = asyncio.Queue()
    def __aiter__(self): return self
    async def __anext__(self):
        value = await self.incoming.get()
        if value is None: raise StopAsyncIteration
        return json.dumps(value)
    async def send(self, raw): await self.sent.put(json.loads(raw))

class HandlerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.proxy = StreamProxy()
        self.provider = Mock()
        self.handler = MCPHandler(self.proxy, self.provider, self.provider)
    async def asyncTearDown(self):
        self.handler.close()
        await self.proxy.stop()

    async def test_invalid_arguments_are_errors(self):
        for arguments in ([], {'query':[]}, {'query':'x','source':'bogus'}, {'query':'x'*257}):
            self.assertTrue((await self.handler.handle_call_tool('play_song', arguments))['isError'])

    async def test_play_result_uses_ticket_and_no_system_directive(self):
        self.provider.search_and_extract.return_value = {'title':'Test','artist':'Artist','audio_url':'https://example.com/audio','source':'YouTube'}
        reply = await self.handler.handle_call_tool('play_song', {'query':'Test'})
        data = payload(reply)
        self.assertEqual(data['status'], 'ready')
        self.assertIn('/stream/', data['stream_url'])
        self.assertEqual(data['next_tool'], 'self.audio.play_url')
        self.assertNotIn('HỆ THỐNG', reply['content'][0]['text'])

    async def test_resources_read_is_not_exposed(self):
        reply = await dispatch({'id':1,'method':'resources/read','params':{'url':'http://127.0.0.1'}}, self.handler)
        self.assertEqual(reply['error']['code'], -32601)

    async def test_ping_stop_and_duplicate_during_slow_search(self):
        release = threading.Event()
        started = threading.Event()
        def slow(*args):
            started.set()
            release.wait(3)
            return {'title':'Test','artist':'Artist','audio_url':'https://example.com/audio'}
        self.provider.search_and_extract.side_effect = slow
        ws = FakeWebSocket()
        task = asyncio.create_task(serve_connection(ws, self.handler))
        request = {'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':'play_song','arguments':{'query':'Test'}}}
        try:
            await ws.incoming.put(request)
            while not started.is_set(): await asyncio.sleep(.01)
            await ws.incoming.put(request)
            await ws.incoming.put({'jsonrpc':'2.0','id':2,'method':'ping'})
            await ws.incoming.put({'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':'stop_music'}})
            replies = [await asyncio.wait_for(ws.sent.get(), 1), await asyncio.wait_for(ws.sent.get(), 1)]
            self.assertEqual({r['id'] for r in replies}, {2,3})
            release.set()
            reply = await asyncio.wait_for(ws.sent.get(), 2)
            self.assertEqual(payload(reply['result'])['status'], 'cancelled')
            self.assertEqual(self.provider.search_and_extract.call_count, 1)
            self.assertEqual(self.proxy.current_track['status'], 'stopped')
            await ws.incoming.put(request)
            cached = await asyncio.wait_for(ws.sent.get(), 1)
            self.assertEqual(cached, reply)
        finally:
            release.set()
            await ws.incoming.put(None)
            await task

if __name__ == '__main__': unittest.main()
