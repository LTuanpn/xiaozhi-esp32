# Bảng điều khiển LAN và mic điện thoại

1. Board TFT cung cấp trang điều khiển. QR chứa mã ghép cặp ngẫu nhiên dùng một lần. Trang dùng tool MCP hiện có theo danh sách cho phép và đọc trạng thái thật.
2. Giữ QR cấu hình Wi-Fi riêng. OTA chỉ lưu endpoint khi người dùng xác nhận, rồi có thao tác khởi động lại riêng. Quản lý bản phát hành dùng Fleet.
3. Mic điện thoại chỉ bật trên origin HTTPS tin cậy. Reverse proxy chuyển cùng origin đến ESP32. Gửi PCM mono 16 kHz theo frame 60 ms để dùng Opus encoder và kết nối AI của robot.
4. Một phiên nhấn giữ tại một thời điểm, có giới hạn bộ đệm, thời lượng và timeout. Dừng thu mic robot khi nhận phiên; trả WakeNet khi nhả hoặc mất kết nối. Ưu tiên GUARD và báo động.
5. Kiểm tra JS, host test và CI build. Nạp và thử phát/thu trên robot ở lượt sau theo yêu cầu.

## Thiết lập khi thử

- Kết nối robot và điện thoại vào cùng LAN. Nói “Jarvis, mở trang điều khiển” để gọi `self.screen.open_lan_control`. Nếu Xiaozhi Cloud cần quy tắc định tuyến, gắn câu này với tool mới và giữ `self.screen.show_qr` cho cấu hình Wi-Fi. QR điều khiển có hiệu lực hai phút. Phiên điều khiển trên tab có hiệu lực một giờ.
- Bảng điều khiển ban đầu ở `http://<IP-robot>:8080`. Kiểm tra âm lượng, độ sáng, trạng thái, hẹn giờ, GUARD và biểu cảm tại đây. Dùng HTTP trên LAN tin cậy. Giữ cổng 8080 trong LAN.
- Để dùng mic điện thoại, chạy HTTPS reverse proxy trên máy cùng LAN. Dùng tên miền trỏ về IP LAN của máy đó, chứng chỉ được điện thoại tin cậy và IP LAN ổn định cho robot. Ví dụ Caddyfile cho máy đã có chứng chỉ và DNS phù hợp.

  ```caddyfile
  jarvis.example.vn {
      tls /path/to/fullchain.pem /path/to/privkey.pem
      reverse_proxy 192.168.1.7:8080
  }
  ```

  Thay tên miền, đường dẫn chứng chỉ và IP robot theo mạng thực tế. Mở HTTPS trên điện thoại để kiểm tra chứng chỉ. Sau đó vào mục HTTPS trong bảng điều khiển HTTP, lưu origin như `https://jarvis.example.vn` và yêu cầu robot mở QR mới.
- Qua HTTPS, bấm cấp quyền mic một lần. Nhấn giữ, chờ nút hiện **Đang thu**, nói rồi nhả để gửi. Robot trả lời qua loa như khi dùng mic tích hợp. GUARD phải tắt và robot ở trạng thái chờ trước khi bắt đầu. Mỗi lượt tối đa 45 giây, kể cả thời gian mở kết nối.
- Khi kiểm thử phần cứng, thử thêm nhả nút ngay lúc đang mở kết nối, tắt Wi-Fi điện thoại giữa lượt, chuyển app, bấm giữ từ hai thiết bị và gọi Jarvis ngay sau khi robot kết thúc trả lời. Kiểm tra riêng WebSocket và MQTT/UDP nếu dùng cả hai.
