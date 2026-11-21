# Phiếu nghiệm thu Jarvis trên robot

**Trạng thái ban đầu: chưa chạy trên thiết bị.** Bản bàn giao để đối chiếu là firmware 2.5.20 cho LCD 240×240 và Music MCP 2.3.0. Tại thời điểm bàn giao, robot chạy 2.5.19 và tiến trình phục vụ chạy 2.2.6. Nếu thử bản nâng cấp tiếp theo, ghi đúng phiên bản thực tế vào phiếu. [Báo cáo và checksum](danh-gia-jarvis-2026-11-21.md) · [Kế hoạch nâng cấp](ke-hoach-nang-cap-jarvis.md)

Ngày và người thử __________ · Board __________ · Firmware/commit __________ · Music MCP __________ · Quy tắc Cloud __________ · Mạng LAN/VPS __________ · Bộ biểu cảm __________

### Điều kiện đầu buổi

- [ ] Lưu cấu hình và bản sao dữ liệu cần phục hồi. Xác minh đúng biến thể board, ảnh ứng dụng và checksum. Ghi bản cũ để có thể quay về.
- [ ] Chuyển server sang bản cần thử, xác nhận phiên bản API, trạng thái kết nối MCP và địa chỉ stream mà robot truy cập được. Nạp firmware bảo toàn NVS. Đọc lại phiên bản trên robot và kiểm tra Wi-Fi đã lưu.
- [ ] Mở log robot và server, ghi múi giờ. Chọn âm lượng nghe được. Với mỗi lỗi ghi giờ, câu người dùng nói, chữ robot nhận nếu thấy, lời robot trả, mã lượt nếu có và mốc cuối cùng. Che token, mật khẩu và vé stream khi chia sẻ log.

### Hồi quy trên mọi bản ứng viên

Điền **đạt / lỗi / chưa chạy** kèm số lượt hoặc mã bằng chứng ở cột cuối. Với ca lỗi, ghi cả kết quả của lần gọi đầu tiên trước khi thử lại.

| Mã | Thao tác | Dấu hiệu đạt | Kết quả / bằng chứng |
|---|---|---|---|
| M1 | Sau boot nguội, nói “Jarvis, mở nhạc Tò Te Tí” một lần. Lặp quy trình riêng với “Chúng Ta Của Hiện Tại”. | Nghe lời chuẩn bị một lần, đúng bài phát ra loa ngay lần yêu cầu đầu. Ghi thời gian từ lúc nói xong tới âm đầu, đối chiếu tìm nguồn, HTTP, PCM và mặt nhạc. | — |
| M2 | Thử lần lượt tên bài không có, tên mơ hồ, nguồn lỗi và mất MCP sau lời chuẩn bị. Khôi phục rồi gọi một bài có thật. | Mỗi ca có một kết quả phù hợp để người nghe biết phải làm gì. Khi thiếu bằng chứng, robot báo chưa có phản hồi. Lượt mới phát được sau phục hồi. | — |
| M3 | Dừng lúc đang tìm, gọi bài B khi bài A đang chờ, rồi gửi hai yêu cầu độc lập giống nhau. | Kết quả muộn của A không phát đè B. Không lặp lời kết trong cùng lượt. Hai lượt độc lập vẫn được tiếp nhận. | — |
| M4 | Thử URL phát hết hạn trước khi mở, rồi cắt mạng giữa bài đang nghe. Thử gọi Jarvis nói chen khi nhạc phát và khi chuông reo. | LCD phân biệt lỗi mở với ngắt giữa bài. Ghi nguyên lời báo và khả năng nghe/ngắt lời. Bản 2.5.20 dùng chung clip lỗi; bản có P0 mục 2 phải đọc được hai lời riêng. | — |
| T1 | Đặt timer “nấu mì” 45 giây, hỏi còn bao lâu, gia hạn 30 giây rồi hỏi lại. Thử thay timer đang chạy, hủy và hỏi lại. | LAN và tool trả đúng nhãn, trạng thái và số giây tại lúc lấy mẫu (dung sai 1 giây). Hỏi không đổi hạn. Thay timer cần xác nhận. Hủy xong không reo. | — |
| T2 | Đặt timer và workout gần cùng hạn, ngắt Internet sau khi đặt. Xác nhận từng chuông bằng BOOT và qua LAN khi LAN còn kết nối. Thử GUARD khi mic rảnh và khi loa bận. | Hai chuông tới theo thứ tự và giữ nhãn. Robot báo trạng thái GUARD đúng khả năng nghe thực tế. Ghi lại hoạt động bị ngắt và trạng thái sau chuông. | — |
| Q1 | Từ chat, mở QR điều khiển rồi yêu cầu đóng. Mở riêng QR Wi-Fi; trên Redmi 11S dùng Máy quét mặc định, ghi kết quả, sau đó đối chiếu với nút quét trong Cài đặt Wi-Fi. Kiểm tra BOOT/thoát và gọi lại Jarvis. | Mỗi lần đúng một QR, đóng xong về đúng màn hình. QR điều khiển mở đúng trang LAN. QR Wi-Fi phải được chấm riêng cho từng trình quét về cả kết nối SSID lẫn mở portal Xiaozhi gốc. Ghi riêng lúc Wi-Fi đã lưu và Jarvis trở lại. | — |
| S1 | Từ chat lần lượt nói “màn hình chờ”, “màn hình chính”. Khởi động với OTA không có bản mới, rồi thử endpoint OTA của môi trường thử không liên lạc được. Cho hiển thị log dài. | Cả hai lệnh thực sự về đồng hồ. Phân biệt chưa có bản mới với lỗi kết nối. Dải chữ in hoa, dài thì chạy, khác màu chữ góc trái, lỗi đỏ và READY cuối màu vàng. | — |
| S2 | Trong Fleet chọn RoboEyes tích hợp rồi một gói biểu cảm khác. Trên mỗi bộ, gọi nhạc, GUARD, timer, workout và một tool trả văn bản. Quan sát từ lúc gọi đến khi về standby. | Robot dùng đúng bộ đã chọn ở thông báo. LCD không lộ chỉ thị như `%emotion`, tên hàm hoặc JSON. Ghi cả câu nói lặp nếu có. | — |
| D1 | Hỏi riêng kết nối Wi-Fi, OTA, MCP Music ở trạng thái tốt. Làm gián đoạn từng dịch vụ trong môi trường thử, đổi IP LAN rồi hỏi lại sau khi mạng phục hồi. | Trả đúng chặng đã kiểm tra và thời điểm. Trạng thái cũ hoặc chưa xác minh được ghi rõ. Khi Cloud mất, đối chiếu bằng LAN/BOOT. Gọi nhạc lại để kiểm tra đường stream sau đổi IP. | — |
| U1 | Dùng hồ sơ thử để tạo và đọc một ghi chú, thêm món vào danh sách mua sắm, hỏi kho tri thức và một tiện ích trực tuyến. Đổi hồ sơ rồi kiểm tra cách ly. | Lời trả phù hợp dữ liệu đã ghi, đúng hồ sơ. Khi nguồn ngoài lỗi có phản hồi rõ. LCD không lộ nội dung gọi tool. Dọn dữ liệu thử đã tạo. | — |

### Ca bổ sung theo tính năng đã đưa vào bản thử

| Mã | Thao tác và kết quả cần ghi nhận | Kết quả / bằng chứng |
|---|---|---|
| T3 · Phục hồi timer | Mất điện trước hạn, bật lại sau hạn, bật khi chưa có giờ mạng, rồi khởi động lại sau hủy hoặc xác nhận. Đối chiếu nhãn, thời gian, số lần báo quá hạn và trạng thái chờ đồng bộ. | — |
| T4 · Báo lại, nhiều timer | Đặt hai timer có tên, hỏi từng cái, thử trùng tên, báo lại 5 phút, hủy chỉ một cái và để hai cái tới hạn gần nhau. Thử đạt giới hạn số lượng và bật nguồn lại. | — |
| M5 · Nghe tiếp | Ghi vị trí đang nghe, cắt mạng rồi yêu cầu tiếp tục. Đo sai lệch khi nguồn hỗ trợ tua. Với nguồn không hỗ trợ, phải có lựa chọn phát lại. Dừng hoặc đổi bài trong khi chờ. | — |
| V1 · VPS | Khi có VPS và tên miền, đưa robot sang Wi-Fi khác, gọi nhạc qua HTTPS, restart dịch vụ rồi thử lại. Nếu có hai robot, ghép mỗi UUID với đúng phiên và xác nhận âm thanh trên đúng loa. Thử khôi phục và quay lại LAN. | — |
| O1 · Lệnh offline | Chỉ chạy khi có bản nhận lệnh tiếng Việt tại chỗ. Ngắt Internet, thử ba lệnh ngắn trong yên lặng và khi loa đang phát. Ghi số lần nhận đúng, kích hoạt nhầm, bộ nhớ và độ trễ. | — |

### Chốt kết quả

Hai bài M1 là phép thử nhanh. Để chấm lại độ tin cậy, dùng ít nhất **100 lượt trên tối thiểu 20 bài** theo báo cáo. Ghi riêng nhóm mạng tốt, boot nguội, đổi IP và VPS. Trên mạng tốt, mục tiêu đề xuất là ít nhất **95% phát đúng ngay lần đầu** và **95% có tiếng trong 8 giây** từ khi nói xong. Giữ các ca chủ động gây lỗi ở nhóm riêng để đánh giá câu trả lời, số lượt im lặng, sai bài và lặp câu.

| Bản / môi trường | Số lượt hợp lệ | Đúng bài lần đầu | Có tiếng ≤ 8 giây | Im lặng / sai bài / lặp | Ca lỗi, ca chưa chạy và việc tiếp theo |
|---|---:|---:|---:|---|---|
| __________ | | | | | |
