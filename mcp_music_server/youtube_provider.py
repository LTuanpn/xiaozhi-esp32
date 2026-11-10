import sys
import logging
import yt_dlp

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if sys.stderr and hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

logger = logging.getLogger("YouTubeProvider")

class YouTubeProvider:
    def __init__(self):
        self.ydl_opts = {
            'format': 'bestaudio[ext=m4a]/bestaudio[ext=webm]/bestaudio/best',
            'quiet': True,
            'no_warnings': True,
            'ignoreerrors': True,
            'noplaylist': True,
            'extract_flat': False,
            'skip_download': True,
        }

    def search_and_extract(self, query: str):
        """
        Tìm kiếm bài hát trên YouTube và trích xuất direct audio URL cùng metadata.
        Hỗ trợ cả tên bài hát hoặc link YouTube trực tiếp.
        """
        target = query.strip()
        if not (target.startswith("http://") or target.startswith("https://")):
            target = f"ytsearch1:{target}"

        logger.info(f"Đang tìm kiếm YouTube cho: '{query}'...")
        try:
            with yt_dlp.YoutubeDL(self.ydl_opts) as ydl:
                info = ydl.extract_info(target, download=False)
                if not info:
                    logger.warning(f"Không tìm thấy kết quả YouTube cho: '{query}'")
                    return None

                # Nếu là danh sách tìm kiếm ytsearch
                if 'entries' in info:
                    entries = [e for e in info['entries'] if e]
                    if not entries:
                        logger.warning(f"Không có video nào trong kết quả tìm kiếm cho: '{query}'")
                        return None
                    entry = entries[0]
                else:
                    entry = info

                title = entry.get('title', 'Unknown Title')
                duration = entry.get('duration', 0)
                uploader = entry.get('uploader') or entry.get('channel') or 'YouTube'
                webpage_url = entry.get('webpage_url') or entry.get('url')
                audio_url = entry.get('url')
                thumbnail = entry.get('thumbnail', '')
                user_agent = entry.get('http_headers', {}).get('User-Agent', '')

                # Định dạng thời lượng mm:ss
                mins, secs = divmod(duration, 60)
                duration_str = f"{mins:02d}:{secs:02d}"

                logger.info(f"Tìm thấy: {title} ({duration_str}) - {uploader}")
                return {
                    'source': 'YouTube',
                    'title': title,
                    'artist': uploader,
                    'duration': duration,
                    'duration_str': duration_str,
                    'audio_url': audio_url,
                    'webpage_url': webpage_url,
                    'thumbnail': thumbnail,
                    'user_agent': user_agent
                }
        except Exception as e:
            logger.error(f"Lỗi khi trích xuất YouTube: {e}")
            return None

    def search_multiple(self, query: str, limit: int = 5):
        """Tìm kiếm nhiều kết quả để tra cứu (chỉ lấy metadata, không lấy direct stream)."""
        opts = {
            'quiet': True,
            'no_warnings': True,
            'ignoreerrors': True,
            'extract_flat': True,
            'skip_download': True,
        }
        target = f"ytsearch{limit}:{query}"
        results = []
        try:
            with yt_dlp.YoutubeDL(opts) as ydl:
                info = ydl.extract_info(target, download=False)
                if info and 'entries' in info:
                    for e in info['entries']:
                        if not e:
                            continue
                        dur = e.get('duration') or 0
                        mins, secs = divmod(dur, 60)
                        results.append({
                            'source': 'YouTube',
                            'title': e.get('title', ''),
                            'artist': e.get('uploader', '') or 'YouTube',
                            'duration_str': f"{mins:02d}:{secs:02d}" if dur else "N/A",
                            'url': e.get('url') or e.get('webpage_url') or ''
                        })
        except Exception as e:
            logger.error(f"Lỗi tìm kiếm danh sách YouTube: {e}")
        return results
