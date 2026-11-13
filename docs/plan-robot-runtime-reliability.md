# Kế hoạch sửa QR, nhạc, mic và báo động

- QR cấu hình mở hotspot Xiaozhi và hiển thị URL thực của portal kèm tên hotspot. QR trang web vẫn nhận URL người dùng đưa.
- Giữ WakeNet trong khi phát và trong khoảng nghỉ báo động. Kiểm tra đường kết thúc luồng, lỗi kết nối AI và chuyển về idle qua log.
- Timer/GUARD phát chuông và nhiều câu tiếng Việt theo lượt, có khoảng yên để gọi Jarvis. Dùng câu thu sẵn để hoạt động khi mất mạng. GUARD chỉ báo tiếng động bất thường vì cảm biến chưa xác nhận được xâm nhập.
- Đưa dashboard và dữ liệu về thư mục server. Di chuyển SQLite khi server đã dừng, giữ nguyên URL API, khóa và dữ liệu. Web lưu URL OTA qua COM sau khi đối chiếu UUID.
- Kiểm thử server, frontend, build TFT và xác minh trên COM. Quét QR, nghe chuông và thử wake word bằng giọng thật trên robot.
