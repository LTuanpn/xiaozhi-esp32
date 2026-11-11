import sys
import logging
import urllib.parse
import requests
import yt_dlp
from youtube_provider import YouTubeProvider, QuietExtractorLog
from music_selection import choose_candidate, AmbiguousMusic

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if sys.stderr and hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

logger = logging.getLogger("ZingMP3Provider")

class ZingMP3Provider:
    def __init__(self, youtube_provider: YouTubeProvider = None):
        self.youtube_provider = youtube_provider or YouTubeProvider()
        self.session = requests.Session()
        self.session.headers.update({
            'User-Agent': (
                'Mozilla/5.0 (Windows NT 10.0; Win64; x64) '
                'AppleWebKit/537.36 (KHTML, like Gecko) '
                'Chrome/124.0.0.0 Safari/537.36'
            )
        })

    def search_suggestions(self, query: str):
        """Tìm kiếm gợi ý bài hát từ Zing MP3 API."""
        encoded_query = urllib.parse.quote_plus(query.strip())
        url = f"https://ac.zingmp3.vn/v1/web/ac-suggestions?query={encoded_query}"
        try:
            res = self.session.get(url, timeout=5)
            if res.status_code == 200:
                data = res.json()
                items = data.get('data', {}).get('items', [])
                songs = []
                for item in items:
                    if 'suggestions' in item:
                        for s in item['suggestions']:
                            # type 1 là bài hát (song)
                            if s.get('type') == 1:
                                artists = ", ".join([a.get('name', '') for a in s.get('artists', [])])
                                songs.append({
                                    'id': s.get('id'),
                                    'title': s.get('title'),
                                    'artist': artists,
                                    'duration': s.get('duration', 0),
                                    'link': s.get('link'),
                                    'thumb': s.get('thumb')
                                })
                return songs
        except Exception:
            logger.warning("Zing MP3 search failed")
        return []

    def search_and_extract(self, query: str, artist: str = ""):
        """
        Tìm kiếm và trích xuất audio bài hát Zing MP3.
        Nếu bài hát bị hạn chế bản quyền VIP, tự động fallback sang YouTube.
        """
        logger.info(f"Đang tìm kiếm Zing MP3 cho: '{query}'...")
        suggestions = self.search_suggestions((query + " " + artist).strip())
        
        target_title = query
        target_artist = ""
        zing_link = None
        duration = 0

        if suggestions:
            first, alternatives = choose_candidate(query, artist, suggestions)
            if first is None:
                raise AmbiguousMusic(alternatives)
            target_title = first['title']
            target_artist = first['artist']
            zing_link = urllib.parse.urljoin('https://zingmp3.vn/', first.get('link') or '')
            parsed = urllib.parse.urlsplit(zing_link)
            if parsed.scheme != 'https' or parsed.hostname != 'zingmp3.vn' or not parsed.path.startswith('/bai-hat/'):
                zing_link = None
            duration = first['duration']
            logger.info(f"Zing MP3 tìm thấy: {target_title} - {target_artist}")

        # Thử trích xuất từ link Zing MP3 nếu có
        if zing_link:
            try:
                ydl_opts = {
                    'format': 'bestaudio/best',
                    'quiet': True,
                    'no_warnings': True,
                    'ignoreerrors': False,
                    'noplaylist': True,
                    'skip_download': True,
                    'socket_timeout': 8, 'retries': 1, 'extractor_retries': 1,
                    'logger': QuietExtractorLog(),
                }
                with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                    info = ydl.extract_info(zing_link, download=False)
                    if info and info.get('url'):
                        mins, secs = divmod(info.get('duration') or duration, 60)
                        return {
                            'source': 'Zing MP3',
                            'title': info.get('title') or target_title,
                            'artist': target_artist or 'Zing MP3',
                            'duration': info.get('duration') or duration,
                            'duration_str': f"{mins:02d}:{secs:02d}",
                            'audio_url': info.get('url'),
                            'webpage_url': zing_link,
                            'thumbnail': info.get('thumbnail') or ''
                        }
            except Exception:
                logger.warning("Zing MP3 unavailable; trying YouTube")

        # Fallback sang YouTube với tên bài hát + ca sĩ
        fallback_query = f"{target_title} {target_artist}".strip()
        logger.info(f"Fallback YouTube: Tìm kiếm '{fallback_query}'...")
        yt_result = self.youtube_provider.search_and_extract(target_title, target_artist or artist)
        if yt_result:
            yt_result['note'] = '(Chuyển tiếp từ Zing MP3 sang YouTube chất lượng cao)'
            return yt_result

        return None
