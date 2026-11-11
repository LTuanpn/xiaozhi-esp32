import logging
from urllib.parse import urlsplit, parse_qs
import yt_dlp
from music_selection import choose_candidate, AmbiguousMusic

logger = logging.getLogger(__name__)

class QuietExtractorLog:
    def debug(self, message): pass
    def warning(self, message): pass
    def error(self, message): logger.warning('Music extractor failed; details omitted to protect signed URLs')

class YouTubeProvider:
    def __init__(self):
        self.ydl_opts = {'format': 'bestaudio[ext=m4a]/bestaudio[ext=webm]/bestaudio/best',
            'quiet': True, 'no_warnings': True, 'ignoreerrors': False, 'noplaylist': True,
            'skip_download': True, 'socket_timeout': 8, 'retries': 1, 'extractor_retries': 1,
            'logger': QuietExtractorLog()}

    @staticmethod
    def video_url(value):
        parsed = urlsplit(value)
        if parsed.scheme not in ('http', 'https'):
            return None
        host = (parsed.hostname or '').lower()
        if host in ('youtube.com', 'www.youtube.com', 'music.youtube.com', 'm.youtube.com'):
            video = parse_qs(parsed.query).get('v', [''])[0]
            if not video and parsed.path.startswith(('/shorts/', '/embed/')):
                video = parsed.path.split('/')[2]
        elif host == 'youtu.be':
            video = parsed.path.strip('/').split('/')[0]
        else:
            raise ValueError('Only YouTube video links are supported')
        import re
        if not re.fullmatch(r'[A-Za-z0-9_-]{11}', video):
            raise ValueError('Invalid YouTube video link')
        return 'https://www.youtube.com/watch?v=' + video

    def search_multiple(self, query, limit=5):
        opts = {**self.ydl_opts, 'extract_flat': True}
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(f'ytsearch{min(5, limit)}:{query}', download=False)
        rows = []
        for entry in (info or {}).get('entries', []):
            if not entry: continue
            duration = int(entry.get('duration') or 0)
            mins, secs = divmod(duration, 60)
            rows.append({'source': 'YouTube', 'video_id': entry.get('id'),
                'title': entry.get('title') or '', 'artist': entry.get('uploader') or entry.get('channel') or '',
                'duration': duration, 'duration_str': f'{mins:02d}:{secs:02d}',
                'is_live': entry.get('is_live', False),
                'url': 'https://www.youtube.com/watch?v=' + (entry.get('id') or '')})
        return rows

    def search_and_extract(self, query, artist=''):
        target = self.video_url(query.strip())
        if target is None:
            rows = self.search_multiple((query + ' ' + artist).strip())
            choice, alternatives = choose_candidate(query, artist, rows)
            if choice is None:
                raise AmbiguousMusic(alternatives)
            target = choice['url']
        with yt_dlp.YoutubeDL(self.ydl_opts) as ydl:
            info = ydl.extract_info(target, download=False)
        if not info or not info.get('url') or info.get('is_live'):
            return None
        duration = int(info.get('duration') or 0)
        mins, secs = divmod(duration, 60)
        logger.info('Selected music video=%s title=%s channel=%s', info.get('id'), info.get('title'), info.get('channel'))
        return {'source': 'YouTube', 'video_id': info.get('id'), 'title': info.get('title') or '',
            'artist': info.get('artist') or info.get('uploader') or info.get('channel') or '',
            'duration': duration, 'duration_str': f'{mins:02d}:{secs:02d}', 'audio_url': info['url'],
            'webpage_url': target, 'user_agent': info.get('http_headers', {}).get('User-Agent', '')}
