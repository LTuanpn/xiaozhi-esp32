# Bluetooth phát âm thanh cho TFT 240×240

Biến thể `bread-compact-wifi-lcd-240x240-kcx` giữ nguyên màn, mic, loa, bảng phân vùng và assets. PCM5102 nhận I²S, KCX nhận line-in và làm bộ phát Bluetooth tới loa. Giọng trả lời và nhạc có lựa chọn ngõ ra riêng. Timer, GUARD và âm hệ thống phát trên loa robot.

## Triển khai

- Mang loại nguồn nhạc qua hàng đợi audio để dùng cùng một bộ giải mã. Biến thể KCX dùng chung BCLK/LRCK, tách DATA DAC và ngõ DATA loa bằng GPIO matrix. Khi chưa có liên kết Bluetooth đã xác nhận, âm thanh phát trên loa robot.
- Quản lý KCX qua UART worker và chân LINK. Trang LAN và tool MCP dùng cùng trạng thái và thao tác. Quét trong khoảng hữu hạn, chọn MAC hoặc tên trùng duy nhất, kiểm tra kết nối và lưu MAC đã xác nhận. Dùng load switch để bật tắt nguồn KCX.
- Build cả board gốc và biến thể KCX. Xác minh phần mềm trước, đo tín hiệu và thử truyền âm thanh sau khi có phần cứng.

## Sơ đồ chân mở rộng

| ESP32-S3 / nguồn | Đích | Ghi chú |
| --- | --- | --- |
| GPIO15 | PCM5102 BCK | Dùng chung clock BCLK với MAX98357A. |
| GPIO16 | PCM5102 LCK/LRCK/WS | Dùng chung clock WS với MAX98357A. |
| GPIO8 | PCM5102 DIN | DATA riêng cho DAC ngoài. |
| GPIO7 | MAX98357A DIN | Chân loa tích hợp giữ nguyên. |
| GPIO9 TX | KCX RX | UART1, logic 3,3 V. |
| GPIO10 RX | KCX TX | UART1, logic 3,3 V. |
| GPIO11 | KCX LINK | Với module LINK mức cao khi kết nối. Kiểm tra mức điện áp và cực tính trên mạch thực tế. |
| GPIO12 | EN của load switch 5 V | Active-high, chọn loại tương thích EN 3,3 V và có điện trở kéo xuống ngoài. |
| 5 V qua load switch | KCX +5V | Dùng load switch riêng. Tuyệt đối không cấp VCC KCX trực tiếp từ GPIO12. |
| GND | GND PCM5102 và PGND của KCX | Chung mass với ESP32. Đi dây analog và mass ngắn. |
| PCM5102 L / R / GND | KCX IN-L / IN-R / AGND | Line out vào line in. Dùng một kênh DAC nếu ngõ còn lại im với cấu hình mono I²S. Không chập hai ngõ ra DAC. |

Mic giữ GPIO4 WS, GPIO5 SCK và GPIO6 SD. Màn hình, USB/COM và nguồn MAX98357A dùng sơ đồ hiện có. Kiểm tra GPIO8–12 thực sự được đưa ra header và chưa dùng trên module thực tế. Với breakout GY-PCM5102, đối chiếu chân VIN, jumper FMT (I²S), XSMT (unmute) và SCK theo sơ đồ đúng revision. Nếu dùng chế độ PLL ba dây của PCM5102A, kéo SCK theo datasheet và cấu hình của breakout. Đo line out/line in và điều chỉnh volume để tránh clipping. Nếu không có load switch, không dùng tính năng bật tắt nguồn như thiết kế này.

## Firmware, trang LAN và lệnh giọng nói

- Build **`bread-compact-wifi-lcd-240x240-kcx`**, nhúng version 2.5.7. Fleet dùng tên biến thể để tránh gán nhầm bản. Robot đang dùng biến thể chuẩn cần chuyển sang KCX một lần. Sau khi có phần cứng, ưu tiên nạp app qua USB vào slot đang boot đã xác minh, hoặc cập nhật app qua kênh quản trị trực tiếp khi đã kiểm tra đúng ảnh. Bảng phân vùng tương thích và phải giữ NVS. Các lần OTA tiếp theo trên biến thể KCX dùng file đúng tên.
- Ghép cặp LAN bằng QR. Trong mục **Loa Bluetooth**, bấm bật, đưa loa vào chế độ pairing, tìm loa và chọn MAC. Có thể điền MAC hoặc tên khớp duy nhất trong kết quả. Bỏ trống để nối lại loa đã lưu. Chọn đường giọng và nhạc riêng. Bluetooth mặc định tắt sau reboot. MAC và đường phát được giữ, bật lại sẽ thử nối loa đã lưu.
- Tool MCP trên thiết bị gồm `self.bluetooth.power`, `self.bluetooth.device`, `self.bluetooth.status` và `self.bluetooth.route`. Lệnh tức thời chỉ trả kết quả đã nhận yêu cầu. Robot xác nhận đã kết nối khi status có `connected: true`. Câu ví dụ “Jarvis, cho nhạc ra Bluetooth, giọng vẫn ở loa robot” dùng `voice=local`, `music=bluetooth`. Lời Cloud/TTS và nhạc được định tuyến theo nguồn. Mic vẫn lấy từ mic tích hợp, các lượt nói và phát nhạc giữ cơ chế ngắt và tiếp tục hiện tại của firmware.
- KCX phụ thuộc revision. Bản này thử UART 115200 và 9600, có và không có CRLF, dùng `AT+SCAN`, `AT+CONADD=0x…`, `AT+DISCON` và xác nhận MAC cùng chân LINK. Tài liệu thử nghiệm v1.7 ghi nhận `AT+SCAN` có thể bị từ chối. Có thể thử nhập MAC thủ công khi quét không hoạt động. Nếu firmware KCX không hỗ trợ kết nối theo MAC hoặc phản hồi không đủ để xác thực, tiếp tục dùng loa robot. `AT+PAIR` có thể tự chọn loa khác, nên không dùng làm fallback. Lưu log UART của mạch thực để bổ sung bộ lệnh tương ứng khi cần.

## Checklist trên bàn thử

1. Chụp silkscreen hai mạch, xác nhận đúng phiên bản, chân 5 V, logic UART và chân LINK. Dùng đồng hồ kiểm tra kéo xuống EN và mức logic trước khi nối vào ESP32.
2. Flash ảnh KCX khi đã sao lưu NVS và xác minh phân vùng. Kiểm tra boot, màn, mic, LAN và âm loa tích hợp với Bluetooth tắt.
3. Qua LAN thử bật và tắt nguồn. Kiểm tra UART probe và phản hồi chế độ TX/analog. Quét loa ở pairing mode, thử khi có hai loa trùng tên, chọn MAC, xác nhận LINK và nối lại loa đã lưu. Đo xem có cấp nguồn ngược qua UART lúc off không.
4. Phát giọng và nhạc ở cả bốn cấu hình local/Bluetooth, thử đổi ngõ khi đang phát và tắt loa giữa bài. Kiểm tra âm lượng, tiếng bụp, mất tiếng do chuyển DMA và độ trễ Bluetooth. Timer và GUARD phải vẫn phát ở loa robot.
5. Kiểm tra WakeNet, nhận lệnh dừng, ngắt nhạc và kết nối lại Wi-Fi. Đối chiếu OTA và danh sách gói Studio sau khi Fleet nhận biến thể mới. Firmware build thành công chỉ xác nhận phần mềm biên dịch.

Tài liệu kỹ thuật. [PCM5102A của TI](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf). [GPIO ESP32-S3 của Espressif](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/peripherals/gpio.html). [Sơ đồ và datasheet KCX](https://offer-product.oss-cn-beijing.aliyuncs.com/product/offer/attachment/3498289377/file/subPdf_781037_345702_20240326-095838418.pdf). [Mã kiểm tra KCX v1.7 và ghi chú UART](https://github.com/Mark-MDO47/BluetoothAudioTransmitter_KCX_BT_EMITTER/tree/KCX_BT_EMITTER_V1.7).
