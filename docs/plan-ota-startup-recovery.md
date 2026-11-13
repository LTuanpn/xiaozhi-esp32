# Sửa khởi động khi OTA mất kết nối

Nguyên nhân: URL LAN lưu trong NVS không thay đổi khi máy đổi mạng; retry tăng gấp đôi chặn InitializeProtocol khoảng 85 phút.

- Giới hạn retry khởi động và timeout HTTP; không báo âm thanh lặp khi đã có cấu hình AI.
- Lưu lựa chọn transport sau lần OTA thành công, dùng lại cấu hình NVS khi OTA lỗi; hỗ trợ firmware cũ chưa có lựa chọn lưu.
- Làm rõ URL máy chủ trên web; tránh cache API.
- Chạy host tests và frontend build; xác minh khả năng build firmware và COM5 trước khi triển khai.

Không thay asset hoặc xóa NVS. Kiểm thử phần cứng cần bản firmware mới: OTA mất mạng, OTA bình thường, wake word và reconnect.
