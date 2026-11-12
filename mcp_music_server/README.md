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

## Dashboard Xiaozhi Fleet

Chạy cùng tiến trình Music MCP. Trên Windows, từ thư mục gốc:

```powershell
npm --prefix fleet-ui ci
npm --prefix fleet-ui run build
py -3.13 -m pip install -r mcp_music_server/requirements.txt
$env:FLEET_ENABLED = "1"
py -3.13 mcp_music_server/server.py
```

Mở http://127.0.0.1:8766 trên máy chạy server. Có thể thêm `FLEET_ENABLED=1` vào `.env` để các lần khởi động sau luôn bật trang. Dữ liệu gán bản, khóa OTA và file firmware tải về nằm trong `.fleet-data/`. Dashboard chỉ lắng nghe localhost. Dùng đường hầm SSH hoặc VPN riêng khi cần quản trị từ máy khác.

1. Trong mục **Thiết bị**, sao chép URL OTA rồi điền vào ô OTA của robot. Khi thử trên LAN, dùng IP của máy chủ và cổng stream (mặc định 8765). Nếu đổi Wi‑Fi làm IP thay đổi, cập nhật URL đã lưu trên robot. Trên VPS, đặt `FLEET_PUBLIC_URL` bằng tên miền HTTPS mà robot truy cập được. Reverse proxy chỉ cần chuyển tiếp đường dẫn `/fleet/ota/` tới cổng stream. URL OTA chứa khóa dùng chung. Chỉ dùng HTTP trên LAN tin cậy.
2. Backend chuyển tiếp kiểm tra OTA và kích hoạt tới URL Xiaozhi cũ. Mặc định là `https://api.tenclass.net/xiaozhi/ota/`. Nếu robot dùng backend khác, đặt `FLEET_UPSTREAM_OTA` bằng URL OTA trước đó. Robot báo lại lần kiểm tra và phiên bản mỗi khi chạy luồng OTA lúc khởi động.
3. Đăng ảnh ứng dụng `xiaozhi.bin` từ build lên GitHub Releases với tên theo đúng mẫu, ví dụ `bread-compact-wifi-lcd-240x240.bin`. Phiên bản nhúng dạng `x.y.z` phải mới hơn bản đang chạy. Dùng cờ prerelease của GitHub để đánh dấu thử nghiệm. Đặt `FLEET_GITHUB_REPO` nếu dùng repo công khai khác.
4. Bấm **Đồng bộ GitHub**, chọn robot và bản, rồi bấm **Gán firmware**. Việc gán có hiệu lực ở lần robot kiểm tra tiếp theo. “Đã gửi bản cập nhật” nghĩa là endpoint đã trả URL. “Đã xác nhận cài” yêu cầu robot báo đúng ELF SHA-256 của bản mới. Bỏ gán sẽ trả robot về chính sách OTA của upstream.
5. Mục **MCP & công cụ** hiển thị trạng thái kết nối Music MCP và các tool nhạc. Để đọc tool thật của robot LCD, nối USB Serial với máy chủ, chọn cổng như `COM5` rồi bấm **Đọc công cụ**. Fleet cũng ghi nhận phiên bản hiện tại qua USB và hiển thị nguồn ghi nhận. Music MCP của Xiaozhi Cloud không cung cấp đường gọi ngược vào mọi robot. Các thao tác OTA tự động của dashboard đi qua endpoint nói trên.

Tích hợp thêm tool nhạc thông qua danh sách và dispatcher của `MCPHandler`. Dịch vụ MCP khác có thể chạy riêng bằng endpoint của nó. Không đưa khóa MCP, token quản trị hoặc file `.fleet-data` lên Git.

### Cập nhật Fleet 2.1

Giải nén `xiaozhi-fleet-2.1.0.zip` vào root repo. ZIP chứa frontend đã build,
mã server, hai gói giao diện và trình đọc assets. Dừng rồi khởi động lại
Music MCP để nạp bản mới. Thư mục `.env` và database Fleet không có trong ZIP.

- Đổi tên robot trong bảng **Thiết bị**. Tên lưu theo UUID và chỉ hiển thị trong Fleet.
- Cột **Kết nối OTA** cho biết robot đã gửi check-in đến Fleet hay chưa và thời điểm gửi gần nhất. Firmware 2.5.4 kiểm tra lúc khởi động, nên Fleet chưa có heartbeat để xác định online/offline liên tục.
- Tab **Giao diện** đọc PNG theo kích thước gốc từ hai gói assets. RoboEyes và đồng hồ chờ của TFT nằm trong firmware. Hai gói giữ nguyên font và wake word. Gói xanh ngọc đổi màu PNG, còn gói gốc dùng để khôi phục tài nguyên.
- Chọn đúng robot và cổng USB trước khi bấm cập nhật tài nguyên. Server dùng Serial để đặt URL và yêu cầu reboot. Robot tự tải qua Wi‑Fi và ghi đè phân vùng assets duy nhất. Chỉ hỗ trợ các build LCD 240×240 đã xác minh. Kiểm tra kết quả trên robot sau reboot.
- Mục MCP có bộ lọc và phân trang. Máy chủ cung cấp 50 tool offline trong 10 nhóm ngoài nhóm nhạc. Kết nối lại MCP để Cloud đọc danh sách mới.
