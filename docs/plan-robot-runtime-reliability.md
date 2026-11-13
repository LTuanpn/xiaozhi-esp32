# Kế hoạch sửa QR, nhạc, mic và báo động

- QR cấu hình mở hotspot Xiaozhi và hiển thị URL thực của portal kèm tên hotspot. QR trang web vẫn nhận URL người dùng đưa.
- Giữ WakeNet trong khi phát và trong khoảng nghỉ báo động. Kiểm tra đường kết thúc luồng, lỗi kết nối AI và chuyển về idle qua log.
- Timer/GUARD phát chuông và nhiều câu tiếng Việt theo lượt, có khoảng yên để gọi Jarvis. Dùng câu thu sẵn để hoạt động khi mất mạng. GUARD chỉ báo tiếng động bất thường vì cảm biến chưa xác nhận được xâm nhập.
- Đưa dashboard và dữ liệu về thư mục server. Di chuyển SQLite khi server đã dừng, giữ nguyên URL API, khóa và dữ liệu. Web lưu URL OTA qua COM sau khi đối chiếu UUID.
- Kiểm thử server, frontend, build TFT và xác minh trên COM. Quét QR, nghe chuông và thử wake word bằng giọng thật trên robot.


## Kiểm thử triển khai

- CI 43408645903 đạt 5 cấu hình firmware và host tests. 27 test server local và build frontend đạt.
- Firmware 5500824 đã nạp vào ota_1 qua COM5, đối chiếu ELF SHA và sao lưu NVS. Asset gốc của bản build khớp với gói đã lưu.
- Fleet chạy lại ở cổng 8766 và Music MCP kết nối sau khi di chuyển dữ liệu. Dashboard trên desktop/mobile và API lưu OTA thật qua COM5 đã thử. URL sai và Origin không hợp lệ bị từ chối. Test backend kiểm tra UUID không khớp.
- Trên robot, QR trang web vẫn cho phép WakeNet chạy. QR cấu hình mở AP Xiaozhi-C631 và portal http://192.168.4.1 theo log.
- Timer lặp, WakeNet chạy trong báo động, sau đó robot chuyển sang nghe lệnh và báo động đã tắt.
- Come My Way phát đến hết 3:55. Log xác nhận robot mở luồng và server báo finished. WakeNet bật trong lúc phát và sau khi về chờ.
- GUARD phát ít nhất hai lượt và WakeNet vẫn bật. Kết thúc bài kiểm thử bằng khởi động lại; chưa xác nhận dừng GUARD bằng giọng nói.
- API lưu URL Fleet và yêu cầu reboot qua COM5 thành công. Robot vào lại Wi-Fi, liên hệ Fleet và WakeNet sẵn sàng. Chưa xác nhận quét portal bằng điện thoại.
- Log server hiện nằm tại `mcp_music_server/logs/fleet.err.log`.

## QR LAN và tiếng chuông ở mỗi lượt

- Khi đang kết nối Wi-Fi, mở trang trạng thái chỉ đọc ngay trên robot qua cổng 8080. QR mở trang này và giữ kết nối AI. Hotspot tiếp tục dùng cho lần thiết lập Wi-Fi.
- Dùng lại 2,32 giây chuông Timer và 3,19 giây còi GUARD trước lời nhắc thứ hai và thứ ba. Lặp cả ba câu, có khoảng yên sau mỗi lượt. Xác minh Ogg Opus và log qua nhiều lượt.
- Giọng Cloud cần API TTS theo giọng của agent. Giữ bản tiếng Việt offline cho đến khi có giao diện tích hợp đã xác minh.
- Build biến thể TFT, sao lưu NVS, nạp ứng dụng qua COM5 và kiểm tra trang LAN từ máy khác với ESP32.
