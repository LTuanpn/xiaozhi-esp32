"""Runtime configuration. Credentials live in the ignored root .env or environment."""
import os
import socket
from pathlib import Path

env_file = Path(__file__).resolve().parents[1] / ".env"
if env_file.exists():
    for line in env_file.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.strip().partition("=")
        if sep and key in {"MCP_ENDPOINT", "STREAM_HOST", "STREAM_PORT", "MUSIC_ADMIN_TOKEN", "FFMPEG_CMD"}:
            os.environ.setdefault(key, value.strip().strip("\"'"))

def get_lan_ip():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"

MCP_ENDPOINT = os.environ.get("MCP_ENDPOINT", "").strip()
STREAM_PORT = int(os.environ.get("STREAM_PORT", "8765"))
LAN_IP = get_lan_ip()
STREAM_HOST = os.environ.get("STREAM_HOST", "").strip() or LAN_IP
STREAM_URL = f"http://{STREAM_HOST}:{STREAM_PORT}/stream.ogg"
ADMIN_TOKEN = os.environ.get("MUSIC_ADMIN_TOKEN", "")
AUDIO_SAMPLE_RATE = 16000
AUDIO_BITRATE = "48k"
AUDIO_CHANNELS = 1
FFMPEG_CMD = os.environ.get("FFMPEG_CMD", "ffmpeg")
