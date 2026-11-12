# Music MCP server

Python 3.13 và FFmpeg có `libopus`. Cài bằng `python -m pip install -r mcp_music_server/requirements.txt`, chạy từ root repo bằng `python mcp_music_server/server.py`.

Tạo `.env` ở root repo (đã được gitignore), hoặc đặt biến môi trường:

```dotenv
MCP_ENDPOINT=wss://your-cloud-endpoint-with-token
STREAM_HOST=192.168.1.100
STREAM_PORT=8765
MUSIC_ADMIN_TOKEN=your-new-random-admin-token
FFMPEG_CMD=ffmpeg
```

`STREAM_HOST` phải là IP/tên máy robot truy cập được; robot và máy server cần thông mạng. Chỉ mở cổng stream trên mạng tin cậy. Không commit endpoint/token. Token cũ từng nằm trong source cần thu hồi và tạo lại trên Xiaozhi Cloud; di chuyển vào `.env` không thu hồi credential đã công khai.

Nếu bỏ trống `STREAM_HOST`, server tự dò lại IP LAN mỗi khi chuẩn bị bài mới, kể cả khi máy vừa đổi Wi-Fi. Nếu đặt IP/tên máy cố định, giá trị đó được giữ nguyên và bạn cần cập nhật khi đổi mạng. Sau khi đổi mạng, robot cũng phải được cấu hình vào mạng mới; cơ chế này không tự sửa Wi-Fi của robot.

`play_song(query, artist, source)` tìm tối đa 5 ứng viên, đánh giá tên/ca sĩ/phiên bản rồi mới lấy audio. Trả `ready` cùng URL riêng cho bài và bước gọi `self.audio.play_url`; đây là nguồn đã chuẩn bị, chưa phải xác nhận loa đã phát. Kết quả mơ hồ trả `needs_clarification`, không tự chọn video khác chủ đề. Alias `play_youtube_music`, `play_zing_mp3` được giữ cho cấu hình Cloud cũ.

Một worker tìm kiếm, tối đa 8 request MCP đang xử lý; timeout không tạo thêm worker vô hạn. Kết quả request được cache theo ID trong từng kết nối (64 ID, 120 giây). `stop_music` hủy ý định tìm bài trước đó, dừng FFmpeg và yêu cầu Cloud gọi `self.audio.stop_playback` để xóa buffer trên robot. Lệnh mới dùng ID mới; không khử trùng theo nội dung câu nói.

Mỗi bài có ticket hết hạn sau 1 giờ và bị vô hiệu khi thay/dừng bài. Proxy chỉ gửi HTTP 200 sau khi FFmpeg có dữ liệu Ogg. `/status` và `POST /stop` yêu cầu `Authorization: Bearer MUSIC_ADMIN_TOKEN`; để trống token thì đóng hai endpoint này. `resources/read` không còn được hỗ trợ. Không dùng URL `/stream.ogg` cũ.

Không có bảo đảm loại mọi quảng cáo: ưu tiên đúng bài/kênh chính thức giảm chọn nhầm, nhưng intro hoặc quảng cáo nhúng trong video vẫn thuộc audio gốc. Ghi nhận `video_id`, tên/kênh và mốc giây nếu gặp lại. Không ghi signed audio URL hay token vào log.

Kiểm thử offline: `python -m unittest discover -s mcp_music_server -v`. Chạy thêm host tests trong `scripts/tests` để kiểm tra QR và bộ phát firmware.

Quy tắc Cloud phù hợp nằm trong [xiaozhi-cloud-rules.md](xiaozhi-cloud-rules.md). Sau khi đổi server/tool, kết nối lại MCP để Cloud nhận schema mới.
