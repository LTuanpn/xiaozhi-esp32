import os
import socket

# WebSocket MCP Endpoint từ Xiaozhi Cloud
MCP_ENDPOINT = (
    "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJ1c2VySWQiOjExNTUxNjEsImFnZW50SWQiOjI2MDAzODAsImVuZHBvaW50SWQiOiJh"
    "Z2VudF8yNjAwMzgwIiwicHVycG9zZSI6Im1jcC1lbmRwb2ludCIsImlhdCI6MTc5NDI5"
    "MTcxNiwiZXhwIjoxODI1ODQ5MzE2fQ.aHNRkGmLE6tGePdn7WuRqRTtyRCLxO8yOfNTy"
    "MAA_whcp9aPb8f3AcoXbHmdmmjhEFLz57PnL6EsdaXlmoUeUA"
)

# Cổng HTTP Audio Stream Proxy cho Robot ESP32
STREAM_PORT = 8765

def get_lan_ip():
    """Tự động xác định địa chỉ IP mạng LAN của máy tính."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return '127.0.0.1'

# Ưu tiên Host tùy chỉnh (nếu dùng Domain/Tunnel) hoặc tự động lấy LAN IP hiện tại
CUSTOM_STREAM_HOST = os.environ.get('STREAM_HOST', '').strip()
STREAM_HOST = CUSTOM_STREAM_HOST if CUSTOM_STREAM_HOST else get_lan_ip()
LAN_IP = get_lan_ip()

STREAM_URL = f"http://{STREAM_HOST}:{STREAM_PORT}/stream.ogg"

# Thông số âm thanh chuẩn cho ESP32 Ogg Opus
AUDIO_SAMPLE_RATE = 16000
AUDIO_BITRATE = "48k"
AUDIO_CHANNELS = 1

# Sử dụng trực tiếp lệnh ffmpeg thông qua biến môi trường PATH của hệ thống
FFMPEG_CMD = "ffmpeg"
