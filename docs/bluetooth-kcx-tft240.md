# Bluetooth phát âm thanh cho TFT 240×240

Biến thể `bread-compact-wifi-lcd-240x240-kcx` giữ nguyên màn, mic, loa, bảng phân vùng và assets. PCM5102 nhận I²S, KCX nhận line-in và làm bộ phát Bluetooth tới loa. Giọng trả lời và nhạc có lựa chọn ngõ ra riêng. Timer, GUARD và âm hệ thống phát trên loa robot.

## Triển khai

- Mang loại nguồn nhạc qua hàng đợi audio để dùng cùng một bộ giải mã. Biến thể KCX dùng chung BCLK/LRCK, tách DATA DAC và ngõ DATA loa bằng GPIO matrix. Khi chưa có liên kết Bluetooth đã xác nhận, âm thanh phát trên loa robot.
- Quản lý KCX qua UART worker và chân LINK. Trang LAN và tool MCP dùng cùng trạng thái và thao tác. Quét trong khoảng hữu hạn, chọn MAC hoặc tên trùng duy nhất, kiểm tra kết nối và lưu MAC đã xác nhận. Dùng load switch để bật tắt nguồn KCX.
- Build cả board gốc và biến thể KCX. Xác minh phần mềm trước, đo tín hiệu và thử truyền âm thanh sau khi có phần cứng.
