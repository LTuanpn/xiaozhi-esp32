# Nạp biểu cảm từ Studio

- Lập lại chỉ mục kho mặc định và kiểm tra truy vấn sơ đồ chân. Các kho hồ sơ riêng giữ quyền truy cập độc lập.
- Thêm công cụ lấy và so sánh giá vàng theo ngày Việt Nam từ lịch sử Vang.Today.
- Dành 2 MiB đầu của phân vùng assets hiện có cho tài nguyên gốc và 6 MiB cuối cho biểu cảm trên TFT 240×240. Giữ bảng phân vùng. Fleet chuyển manifest REX2 sang bảng nhị phân REX3 và giữ nguyên palette, FPS và dữ liệu nén. Ghi dấu hoàn tất sau khi xác minh SHA-256. Khi gói không khả dụng, hiển thị RoboEyes tích hợp.
- Studio cho chọn robot và cài bằng yêu cầu OTA lần khởi động tới. Cả ESP32 và Fleet báo khả năng hỗ trợ qua dữ liệu kiểm tra phiên bản. Dùng chính pipeline preview để kiểm tra kích thước, bố cục và chu kỳ frame.
- Kiểm tra luồng với gói thật, test host, build TFT và xác minh riêng trên thiết bị.
