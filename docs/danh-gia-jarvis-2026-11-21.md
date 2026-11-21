# Đánh giá Jarvis, Music MCP và Fleet

**Ngày 21/11/2026 · Điểm hiện tại 6,8/10 · Ưu tiên sửa độ tin cậy khi các chức năng chạy cùng nhau.**

Mục 1–8 ghi nhận hiện trạng trước nâng cấp. Mục 9 ghi kết quả bàn giao firmware 2.5.20 và Music MCP 2.3.0. Khi triển khai và chạy thử, dùng [kế hoạch nâng cấp tiếp theo](ke-hoach-nang-cap-jarvis.md) và [phiếu nghiệm thu trên robot](kich-ban-thu-jarvis.md).

Phạm vi là robot ESP32-S3 LCD 240×240, firmware 2.5.19 tại commit `6e6c06d`, Music MCP 2.2.6 trong cây mã đang chạy trên Windows, trang điều khiển LAN, Fleet và phần giao tiếp với Xiaozhi Cloud. Đánh giá Cloud dựa trên tool, quy tắc của Jarvis và kết quả quan sát từ hai đầu. Không có log nội bộ của dịch vụ AI để kết luận mô hình đã nghe hoặc suy luận gì trong mọi lượt.

## 1. Điểm theo trải nghiệm người dùng

Điểm là nhận định kỹ thuật. Mức 5 nghĩa là luồng chính chạy nhưng còn phải thao tác lại, mức 7 nghĩa là dùng được hằng ngày và đã xử lý nhiều lỗi, mức 9 đòi hỏi đo được kết quả đầu cuối cả khi mất kết nối, lệnh đến muộn hoặc có hai tác vụ đồng thời. Trọng số ưu tiên các việc người dùng Jarvis thực sự hay gặp. Điểm làm tròn một chữ số, không biểu diễn tỷ lệ yêu cầu thành công.

| Nhóm | Trọng số | Điểm /10 | Cơ sở đánh giá |
|---|---:|---:|---|
| Gọi Jarvis, hội thoại và phản hồi khi gọi tool | 20% | 6,5 | Có wake word, lọc chỉ thị nội bộ, chặn câu trùng trong lượt và lời báo chuẩn bị nhạc. Kết thúc lượt còn phụ thuộc câu chữ AI. |
| Tìm, mở, ngắt nhạc và báo lỗi | 25% | 7,0 | Có xác minh bài, hỏi lại, giới hạn chờ, vé phát, thử mở luồng lại có giới hạn và log FFmpeg. Thiếu xác nhận đầu ra từ robot và số liệu nhiều lượt nói thật. |
| Timer, GUARD, Workout và báo động | 20% | 6,5 | Đếm và báo bằng âm thanh tại robot, kể cả khi Internet mất sau khi đặt. Truy vấn bằng giọng còn thiếu, hai tác vụ dùng chung nhãn, cảnh báo trùng thời điểm có thể bị bỏ. |
| Wi-Fi, QR, OTA và kiểm tra kết nối | 10% | 7,0 | Đã tách QR, dùng portal Xiaozhi gốc, có chẩn đoán mạng và phân biệt OTA có hoặc chưa có bản mới. Máy quét mặc định Redmi 11S vẫn có trường hợp không kết nối được. |
| Màn hình, biểu cảm và điều khiển LAN | 10% | 7,5 | Có đồng hồ, dải chữ chạy, màu lỗi/READY riêng, biểu cảm theo gói và RoboEyes tích hợp. Cần đo lại các tình huống đổi qua lại giữa nhạc, báo động, QR và standby trên LCD. |
| Ghi chú, mua sắm, tri thức và tiện ích trực tuyến | 5% | 7,5 | Có lưu SQLite, phân hồ sơ theo kết nối, trích nguồn và một số phản hồi khi nguồn hỏng. Chưa có số liệu bao phủ giọng thực và dữ liệu ngoài theo thời gian. |
| Fleet, vận hành, nhiều robot và VPS | 10% | 6,5 | Có quản lý OTA, biểu cảm, log và tự nối lại MCP. Health bị giới hạn khi nhiều kết nối, thiếu số liệu dài hạn và bài diễn tập phục hồi trên VPS. |

## 2. Bằng chứng thực tế và giới hạn

- Trong lần rà này, API quản trị nội bộ trả về **23 tool server đang bật**, gồm 6 nhạc, 7 ghi chú/mua sắm, 6 tìm kiếm/kho tri thức và 4 tiện ích Việt Nam. Có một kết nối MCP ở trạng thái `connected`. Fleet ghi nhận bản robot 2.5.19. Server stream lắng nghe cổng 8765, giao diện quản trị lắng nghe tại loopback 8766. Bản ghi thiết bị của Fleet không đủ để khẳng định robot đang online ở đúng thời điểm đọc.
- Log ngày **20/11** có hai lượt robot mở HTTP và server gửi Ogg. Lượt đầu lúc 19:28:35, nguồn kết thúc lúc 19:31:50. Lượt sau mở lúc 19:32:21, nhận byte đầu từ phía server lúc 19:32:22 và nguồn kết thúc lúc 19:36:16. Lượt đầu đã đối chiếu với yêu cầu Tò Te Tí trong điều tra trước. Lịch sử kết nối cũng có một lần lỗi DNS tới Xiaozhi, hiện phiên đã kết nối lại. Các mốc này xác nhận đường truyền tới HTTP client. Mốc âm thanh đầu tiên thực sự ra loa cần đo phía robot.
- Người dùng đã xác nhận truy vấn OTA bằng giọng thành công. Với Redmi 11S, người dùng xác nhận bộ quét trong Cài đặt Wi-Fi vào được Xiaozhi, Máy quét mặc định vẫn lỗi. Chưa đủ bằng chứng để quy lỗi cho một phiên bản ứng dụng cụ thể.
- Đã chạy lại 44 bài kiểm thử Music MCP/Fleet thành công trên Windows. Lệnh và kết quả kiểm tra firmware phía máy phát triển ghi ở cuối báo cáo. Đợt rà này không nạp firmware, không thay cấu hình Cloud, không làm gián đoạn robot hoặc chuyển dịch vụ lên VPS.

## 3. Dữ liệu ở đâu, ai thực hiện, ai trả lời

| Người dùng muốn | Nơi giữ dữ liệu và thực hiện | Lúc bình thường | Khi mất kết nối |
|---|---|---|---|
| Đặt hoặc hỏi timer, workout, GUARD | ESP32 giữ deadline, trạng thái mic và báo động. Web LAN đã ghép đôi đọc được một phần dữ liệu | Giọng nói đi qua Cloud để chọn tool. ESP32 thực thi. Timer/GUARD tự phát câu báo khi đến hạn | Tác vụ đã đặt vẫn chạy khi robot còn nguồn. Web LAN còn dùng được nếu điện thoại tới được robot. Hiểu câu nói tự do offline chưa được xác minh |
| Phát và dừng nhạc | Server chọn nguồn và mã hóa. ESP32 mở HTTP(S), giải mã và phát | ESP32 → AI Xiaozhi → Music MCP → kết quả về AI → tool phát trên ESP32. Dữ liệu âm thanh đi từ nguồn → FFmpeg → ESP32. AI điều phối, robot đọc luồng trực tiếp | Đứt Cloud trước bước giao URL có thể bỏ dở yêu cầu. Đứt stream sau đó ảnh hưởng phát kể cả khi MCP vẫn online. Có câu báo offline trên thiết bị |
| Kiểm tra Wi-Fi, OTA hoặc MCP | Robot đọc IP/RSSI, gọi endpoint OTA hoặc hỏi Fleet về phiên MCP | AI chọn dịch vụ và đọc kết quả từ tool. Kiểm tra OTA theo yêu cầu không tự cài | Wi-Fi còn IP vẫn có thể mất Internet. Health Music dựa vào đường dẫn Fleet đã cấu hình. OTA của hãng hoặc topology khác có thể cho “chưa xác minh” |
| Ghim lời nhắc, lưu ghi chú, mua sắm | Ghim hiện trên LCD. Ghi chú dài hạn và danh sách mua sắm thuộc SQLite trên server | AI cần chọn đúng ý “ghim ở màn hình” hoặc “lưu để hôm sau đọc”. Các robot dùng cùng một endpoint MCP có thể dùng chung hồ sơ | Server tắt thì dữ liệu đã lưu còn trên đĩa, nhưng tool đọc/ghi cần kết nối lại. Chỉ xác nhận sau khi ghi thành công |
| Tin, giá, tài liệu riêng, phạt nguội | Server truy vấn nguồn ngoài hoặc chỉ mục tài liệu riêng | AI tổng hợp kết quả, nguồn và thời điểm. Tool tra phạt có đường dẫn tra thủ công khi nguồn không trả lời | Báo thiếu dữ liệu hoặc nguồn lỗi. Không kết luận “không có vi phạm”, giá bằng 0 hoặc tự gửi nội dung tài liệu riêng lên web |
| QR, màn hình, loa, mic và phần cứng tùy chọn | Robot trực tiếp mở portal, QR, màn hình hoặc điều khiển đầu ra. Bluetooth KCX cần đúng biến thể và module | LAN thao tác tại chỗ, AI gọi theo tool robot công bố. QR điều khiển và QR Wi-Fi có mục đích riêng | Vào portal Wi-Fi gốc tạm ngắt phiên Jarvis. Phải có đường thoát tại thiết bị. Bản 240×240 đang ghi nhận là biến thể thường, Bluetooth KCX chỉ đưa vào kiểm thử biến thể tương ứng |
| OTA, bộ biểu cảm, quản trị | Fleet phân phối, robot tải và xác nhận phiên bản hoặc gói đã cài. Studio có chọn lại RoboEyes tích hợp | Tách “đã giao bản”, “đã tải” và “đã chạy đúng bản”. Các tool giải trí như quay số và chọn ngẫu nhiên chạy trên robot | Giữ giao diện tích hợp khi gói lỗi. Nếu chuyển Fleet lên VPS, chức năng cần cổng USB vẫn phải chạy ở máy cạnh robot |

## 4. Phát hiện nên sửa trước

| Ưu tiên | Điều tìm thấy trong mã | Tác động và thay đổi đề nghị |
|---|---|---|
| P0 | Firmware tính `timer_seconds` từ đồng hồ đơn điệu, làm tròn lên và xuất kèm nhãn qua trạng thái LAN. Tool AI `self.get_device_status` đọc trạng thái Wi-Fi board chung, thiếu các trường đó | “Còn mấy giây?” có thể bị từ chối dù web biết đáp án. Dùng chung phép tính, xuất trường an toàn theo khả năng board. Bổ sung trạng thái GUARD, workout, alarm. Không đưa nguyên đối tượng LAN chứa URL OTA có khóa vào phản hồi AI. [Board LCD](../main/boards/bread-compact-wifi-lcd/compact_wifi_board_lcd.cc) · [Tool trạng thái](../main/mcp_server.cc) |
| P0 | Firmware có hạn chờ nhạc 24 giây. Một câu TTS thiếu các chuỗi đang được kiểm tra như “chuẩn bị” hoặc “đang tìm” có thể xóa hạn chờ. Server có hạn riêng 18 giây cho tìm nguồn phát. Câu báo lỗi local dùng chung clip cho các nguyên nhân | Có đường dẫn dẫn tới im lặng hoặc báo sai chặng khi AI đổi cách nói. Kết thúc chờ theo kết quả, hủy, hết hạn hoặc phát thực tế. Sau hết hạn, bỏ kết quả đến muộn của lượt cũ. Giữ một nơi chịu trách nhiệm nói kết quả cuối. [Điều phối robot](../main/application.cc) · [Tìm nguồn](../mcp_music_server/mcp_handler.py) |
| P0 | `self.audio.stop_playback` lên lịch dừng và trả ngay “Đã dừng phát âm thanh”. Callback có thể bỏ qua thao tác khi alarm đang bật. Mặt nhạc được đặt trước khi robot nhận được âm thanh | Có thể báo thành công dù thao tác chưa xảy ra. Trả đúng “đang có báo động” hoặc kết quả sau thực thi. Hiện “đang mở” trong lúc kết nối. Chỉ hiện “đang phát” sau khi gói âm thanh đầu tiên được đưa ra đường phát. [Tool dừng](../main/boards/bread-compact-wifi-lcd/compact_wifi_board_lcd.cc) · [Bộ phát](../main/notify/notify_player.cc) |
| P0 | Khi workout kết thúc, mã ghi đè `countdown_label_` của timer. Khi đã có một alarm, `StartLocalAlarm` trả false. Timer vừa hết hạn đã xóa deadline trước khi kiểm tra báo thành công | Timer và workout chạy cùng có thể báo nhầm nhãn hoặc mất sự kiện thứ hai. Giữ nhãn theo tác vụ, lưu sự kiện đến hạn chưa báo bằng hàng chờ có giới hạn, định nghĩa thứ tự và cách xác nhận từng báo động. Các nhánh tạo/chạy ESP timer cũng cần kiểm tra mã trả về trước khi xác nhận đã đặt. [Timer và workout](../main/boards/bread-compact-wifi-lcd/compact_wifi_board_lcd.cc) · [Báo động](../main/application.cc) |
| P1 | GUARD có 6 giây ổn định, bỏ qua các mẫu khi robot tự phát âm, cần đường mic hoạt động và tự tắt sau khi kích báo | Có lý do chính đáng để bỏ mẫu, nhưng người dùng đang nghe nhạc có thể tưởng phòng vẫn được theo dõi liên tục. Trả trạng thái “đang ổn định”, “đang nghe”, “tạm ngưng vì loa”, “đã báo, cần bật lại”. Workout hứa bíp mỗi 10 giây trong mô tả tool, callback hiện thấy chỉ cập nhật màn hình theo giây. Kiểm thử bằng mic/loa thật rồi đồng bộ mô tả và hành vi. [GUARD và workout](../main/boards/bread-compact-wifi-lcd/compact_wifi_board_lcd.cc) |
| P1 | Tìm nhạc đã phân biệt mơ hồ, bận, timeout, hủy. Một số lỗi nguồn được quy thành không có kết quả, trong khi ngoại lệ ở nguồn đầu có thể thoát thẳng ra lỗi chung. Chế độ mọi nguồn còn có đường YouTube → Zing → YouTube | Dễ báo “không tìm thấy” hoặc dùng hết thời gian ở một nguồn. Lưu kết quả từng nguồn, tránh thử lại cùng nguồn trong một lượt, phân biệt lỗi kết nối, nguồn từ chối và tìm thành công nhưng không khớp. Giữ lựa chọn chính thức theo bằng chứng, chỉ dùng alias riêng cho ca nhận sai đã quan sát. [Chọn nguồn](../mcp_music_server/mcp_handler.py) · [Zing](../mcp_music_server/zingmp3_provider.py) |
| P1 | Server ghi `ready`, `not_opened` sau 15 giây, `playing` khi ghi byte Ogg đầu, `finished` khi nguồn kết thúc. Robot thử mở luồng nhạc tối đa hai lần trong điều kiện phù hợp trước khi có tiến độ. Khi vé hết hạn hoặc luồng đứt giữa bài, cách phục hồi khác nhau | Thêm mốc đầu ra và lý do kết thúc ở robot, nối với sự kiện server bằng mã lượt không chứa vé. Nếu mạng đứt giữa bài, báo ngắt và cho phép người dùng phát lại. Chỉ thêm tiếp tục từ vị trí cũ khi nguồn và bộ phát hỗ trợ. [Proxy](../mcp_music_server/stream_proxy.py) · [Bộ phát](../main/notify/notify_player.cc) |
| P1 | Health Fleet chỉ xác nhận khi có duy nhất một kết nối MCP. Khi không thấy đúng một card mạng nhận check Fleet gần đây, server có thể dùng tuyến IP mặc định. WebSocket tự thử lại từ 2 đến 30 giây | Câu “Music có kết nối không?” cần tách được robot tới server, phiên server tới Cloud và khả năng mở nguồn. Nhiều robot cần ánh xạ thiết bị với kết nối có chủ đích. Khi chưa biết, nói “chưa xác minh”. Lên VPS dùng tên miền stream ổn định và kiểm thử từ mạng ngoài. [Fleet](../mcp_music_server/fleet.py) · [Kết nối MCP](../mcp_music_server/server.py) |
| P2 | Lọc câu lặp hiện dựa vào nội dung trong lượt. Cache kết quả RPC của server có giới hạn trong một phiên. Hai câu khác chữ cùng xác nhận một hành động hoặc lượt cũ đến sau reconnect vẫn cần kiểm tra | Kiểm thử cả lặp âm thanh và lặp tác dụng như đặt lại timer. Cho phép một câu báo chuẩn bị và một kết quả có ý nghĩa. Dùng mã lượt và trạng thái đã hoàn tất để bỏ phản hồi trễ, vẫn cho phép người dùng chủ động yêu cầu lại. [Lọc hội thoại](../main/conversation_text.h) · [RPC](../mcp_music_server/server.py) |

## 5. Những yêu cầu Jarvis nên xử lý được

Các câu trả lời sau là **thiết kế đề xuất**. Giữ cách xưng hô theo persona đang dùng khi triển khai. AI hiểu câu và gọi tool, thiết bị hoặc server cung cấp số liệu tại thời điểm hỏi. Câu hỏi trạng thái chỉ đọc dữ liệu.

| Người dùng nói | Hành vi mong muốn và nơi thực hiện |
|---|---|
| “Timer nấu mì còn bao nhiêu giây?” | Robot trả số giây và nhãn. AI đọc “Còn 42 giây cho nấu mì”. Khi hết và đang reo, trả “Đã hết giờ, chuông đang reo”. Nếu chưa đặt, trả “Hiện không có hẹn giờ”. |
| “Thêm 30 giây”, “hủy cái hẹn vừa rồi” | ESP32 cộng vào deadline hiện hành hoặc hủy đúng timer. Khi cả workout và timer chạy mà câu nói không rõ, hỏi chọn một lần. Khi người dùng đặt timer thứ hai, hỏi trước khi thay timer đang chạy. |
| “Tôi đang tập được bao lâu, timer bếp còn không?” | Đọc một lần trạng thái hai tác vụ trên robot. Trả cả hai con số từ cùng thời điểm. Việc hỏi không đặt lại đồng hồ. |
| “Phòng còn đang canh không?”, “vừa rồi vì sao có còi?” | Robot trả trạng thái GUARD, loại báo động gần nhất và thời điểm nếu biết. Ví dụ “Vừa phát hiện tiếng động mạnh. Chế độ canh đã dừng sau cảnh báo.” Không suy diễn có người đột nhập. |
| “Đang phát bài nào?”, “sao chưa có tiếng?” | Server cung cấp tên bài, nguồn và bước tìm. Robot cung cấp playback, âm lượng và đầu ra khả dụng. Có thể trả “Đã tìm thấy bài, robot chưa mở được luồng” hoặc “Robot đang phát qua loa ngoài, âm lượng 0”. Chỉ nói về Bluetooth khi board có module. |
| “Hết timer thì sao nếu tôi đang nghe nhạc?” | Báo động được ưu tiên. Khi ngắt nhạc để báo, màn hình nói rõ lý do. Sau đó có thể hỏi “Phát lại bài vừa rồi không?”. Không hứa nối đúng vị trí trước khi hỗ trợ tìm vị trí phát. |
| “Robot có mạng mà sao không mở được bài?” | Đọc theo từng chặng và độ mới của kết quả, ví dụ “Wi-Fi có IP. Server nhạc nối được tới Jarvis. Nguồn phát trả lỗi”. Cho lựa chọn thử lại hoặc tìm bản khác theo lỗi thực. |
| “Lưu ghi chú này rồi ghim lên đồng hồ” | Server xác nhận đã lưu trước, robot xác nhận ghim sau. Nếu một bước hỏng, trả đúng phần đã làm, ví dụ “Đã lưu ghi chú, màn hình đang bận nên chưa ghim được”. Thử lại bước lỗi tránh tạo thêm bản ghi. |
| “Mở QR để điện thoại điều khiển”, “tắt QR”, “mất mạng xem timer ở đâu?” | QR điều khiển giữ Wi-Fi hiện tại. QR Wi-Fi mở portal Xiaozhi và hướng dẫn BOOT để thoát ngay trên LCD. Với Redmi, ghi ngắn tên SSID và cách nối thủ công cạnh mã. Cho xem timer hoặc tắt cảnh báo bằng nút/trang LAN khi khả dụng. |
| “Sau khi mất điện, còn hẹn giờ không?” | Hiện deadline nằm trong RAM. Đề xuất lưu thay đổi khi đặt, sửa, hủy. Khi khởi động có giờ tin cậy mới phục hồi thời hạn tuyệt đối. Nếu đồng hồ chưa đồng bộ, hiển thị “Chưa xác định được thời gian còn lại”. Timer hết lúc tắt máy cần báo quá hạn một lần. |

## 6. Quy tắc phản hồi và chế độ offline

**Hôm nay người dùng thực tế nhận gì?** Tool server trả câu “Tao chưa tìm được bài khớp. Mày nói lại tên bài và ca sĩ giúp tao” khi tìm không ra. Khi nguồn quá hạn, tool trả “Nguồn nhạc phản hồi chậm. Mày thử lại sau nhé.” Các câu này cần phiên Cloud còn hoạt động để đọc. Khi chính robot hết hạn chờ, LCD hiện “Chưa nhận được phản hồi từ máy chủ nhạc.” Nếu đã nhận URL nhưng trình phát thất bại, LCD hiện “Không mở được luồng nhạc từ máy chủ.” Cả hai nhánh local gọi cùng một clip báo nhạc không khả dụng nên lời nghe qua loa không phân biệt nguyên nhân chi tiết như chữ trên màn hình. Với ghi chú, lỗi SQLite trả “Không thể truy cập dữ liệu đã lưu.” Mất Cloud trước khi robot nhận diện được yêu cầu giọng nói thì thiết bị cũng chưa biết người dùng đang hỏi bài nào để báo lỗi riêng cho bài đó. [Phản hồi server](../mcp_music_server/mcp_handler.py) · [Phản hồi robot](../main/application.cc)

**Hành vi đề xuất sau khi nâng cấp**

| Trường hợp | Ai phản hồi | Phản hồi người dùng nên nhận |
|---|---|---|
| Lệnh nhanh, thành công | AI đọc kết quả đã xác nhận, hoặc UI LAN hiển thị khi người dùng bấm | “Còn 42 giây”, “Đã hủy timer nấu mì”. Không cần một lời chuẩn bị cho thao tác tức thời. |
| Tìm bài mất thời gian | ESP32 phát lời chuẩn bị một lần. Server trả trạng thái có cấu trúc | “Đang chuẩn bị nhạc”. Thành công thì bắt đầu phát. Không nói thêm đè lên bài. |
| Không khớp, nhiều bản, hoặc nguồn hỏng | Server trả nguyên nhân. AI nói một lần khi phiên còn sống | “Có hai bản, muốn nghe bản nào?”, “Nguồn nhạc đang lỗi, chưa kiểm tra được bài này”. Chỉ nói “chưa tìm thấy” khi thực sự tìm được dữ liệu nhưng không khớp. |
| Đã báo chuẩn bị rồi mất AI/MCP hoặc lệnh về muộn | ESP32 giữ hạn chờ và báo tại chỗ nếu chưa có kết quả. AI bỏ kết quả muộn thuộc lượt cũ | “Chưa nhận được kết quả phát nhạc”. Chỉ đổ lỗi cụ thể cho server hoặc Wi-Fi khi có bằng chứng. |
| Đã nhận URL nhưng mở lỗi, hoặc đứt giữa bài | ESP32 báo bằng clip local và chữ. Server lưu chặng FFmpeg/HTTP | “Không mở được luồng nhạc” hoặc “Nhạc vừa bị ngắt”. Không phát lại từ đầu ngoài ý muốn. |
| Cloud mất hoàn toàn | LCD, BOOT, chuông và trang LAN khi còn đường nội bộ | Giữ timer/GUARD đã chạy. Bước đầu dùng nút xem hoặc xác nhận. Nếu muốn ra lệnh tiếng Việt hoàn toàn offline, thử riêng bộ nhận lệnh giới hạn và đo RAM, độ trễ, nhận sai trên ESP32-S3. Wake word đơn lẻ chưa đủ cho việc này. |

## 7. Kế hoạch triển khai và tiêu chí nghiệm thu

Ước lượng cho một người, đã có phần cứng, CI và quyền cấu hình Jarvis. Thời gian chờ thử dài ngày, VPS hoặc nhà cung cấp xử lý sự cố nằm ngoài ước lượng. Mục tiêu điểm sau triển khai là **9/10 khi đạt nghiệm thu**, không cộng điểm chỉ vì đã thêm tool.

| Thứ tự | Việc giao | Thời lượng ước tính | Điều kiện chuyển bước |
|---|---|---:|---|
| 1. Sửa timer và trạng thái robot | Firmware xuất trạng thái đọc an toàn qua tool hiện có, thêm nhãn/giây/trạng thái ring, tách workout, kiểm tra lỗi tạo timer, xử lý hai sự kiện đến hạn. LAN và Cloud dùng cùng phép tính. Cập nhật mô tả tool/prompt sau khi firmware hỗ trợ | 2–3 ngày | Đặt, hỏi, gia hạn, hủy, hết hạn, hỏi khi đang reo, timer và workout cùng hết. Chênh số đếm tối đa 1 giây **ở thời điểm lấy mẫu**. Mất Internet sau khi đặt vẫn reo. Lệnh hỏi không thay deadline. Bước gia hạn cần triển khai thao tác sửa trên ESP32. |
| 2. Sửa vòng đời nhạc và lời đáp | Đặt mã lượt từ bước chuẩn bị, truyền qua các đầu có hỗ trợ, giữ tương thích firmware cũ. Tách nguyên nhân theo nguồn. Bổ sung mốc đầu ra robot. Câu báo lỗi có chủ sở hữu, đến hạn thì bỏ kết quả cũ | 3–5 ngày | Thử lỗi DNS, nguồn chậm, không có kết quả, URL sai/hết hạn, FFmpeg lỗi, đang tìm rồi dừng, đổi bài, reconnect và alarm chen ngang. Mỗi lượt hoặc phát đúng, hoặc có một kết quả rõ ràng. Không lộ tool, không nói lặp, không tự phát lại lượt đã hủy. |
| 3. Liên kết màn hình, báo động và LAN | Hiện hoạt động đang chạy trên đồng hồ, truy vấn GUARD/workout, nhận biết trạng thái đang nghe hoặc tạm ngưng. Dùng lại gói biểu cảm đã chọn, giữ lựa chọn RoboEyes. Bổ sung hướng dẫn QR ngay trên LCD | 2–3 ngày | Mở/đóng từng QR về đúng màn hình. Test Redmi bằng cả hai trình quét. Thử timer và GUARD trên bộ tích hợp và gói OTA, chữ dài chạy, READY vàng, lỗi đỏ. Thử wake word khi nhạc và chuông đang phát. |
| 4. Làm rõ health và vận hành server | Tách trạng thái Cloud, endpoint robot truy cập, nguồn âm và FFmpeg. Ghi thời điểm, chặng hỏng và thời gian từ yêu cầu đến đầu ra. Kết nối từng robot với đúng phiên MCP khi có nhiều hồ sơ. Bổ sung kiểm tra phụ thuộc lúc khởi động và kiểm tra khôi phục dữ liệu | 2–3 ngày | Đổi IP LAN, khởi động nguội, mất DNS rồi có lại, hai robot dùng hai kết nối, bài vẫn mở đúng robot. Dashboard phân biệt online hiện tại, kết quả cũ và chưa xác minh. Log không ghi token, vé stream hoặc nội dung ghi chú. |
| 5. Kiểm tra VPS và phát hành thử | Dùng HTTPS cho stream, cấu hình riêng URL Fleet nếu chuyển cả OTA. Giữ admin riêng. Chuyển bản sao DB và hồ sơ cần thiết, giữ một kết nối chủ động cho mỗi token khi chuyển, có đường quay lại máy LAN. USB để tại chỗ | 1–2 ngày + theo dõi | Robot ở Wi-Fi khác vẫn phát đúng, chứng chỉ hợp lệ, VPS khởi động lại tự nối MCP, phục hồi được dữ liệu từ bản sao. Nếu chuyển OTA, thử đúng board, xác nhận bản đang chạy và đường phục hồi trên một robot thử trước. |

**Đo bằng giọng thật trước khi mở rộng.** Đề xuất ít nhất 100 lượt phát trên tối thiểu 20 bài, có Tò Te Tí, Chúng Ta Của Hiện Tại, ca sĩ trùng tên, yêu cầu không có bài và các cách nói khác nhau. Tách nhóm Wi-Fi tốt, boot nguội, LAN đổi IP, VPS và lỗi chủ động. Đích trên mạng tốt là ít nhất 95% lượt có bài xác định phát đúng ngay lần đầu, 95% lượt bắt đầu ra âm thanh trong 8 giây từ lúc nói xong. Báo riêng số lần hỏi lại đúng, số lượt im lặng, sai bài và lặp câu. Ca cố tình gây lỗi phải kết thúc với thông báo đúng lớp hoặc trạng thái chưa xác minh. Không trộn chúng vào tỷ lệ phát thành công. Lưu cả cỡ mẫu và kết quả từng môi trường.

**Trước khi phát hành firmware**, chạy host tests, build board dùng thật và các biến thể bị ảnh hưởng, kiểm tra cả hai transport nếu thay hành vi giao thức chung. Thử có và mất mạng, wake/VAD, nói chen, hai báo động, loa trong, mic, AEC theo build và KCX nếu có phần cứng. Thử lại sau cắt nguồn và đối chiếu trạng thái trên robot, LAN và Dashboard.

**Sau khi đạt các tiêu chí trên**, ưu tiên lưu timer qua mất điện, nhắc lại sau một khoảng thời gian, rồi mới tới nhiều timer có tên. Danh sách phát và nghe tiếp giữa bài nên làm sau khi đã có vị trí phát đáng tin trên robot.

### Các quyết định triển khai để tránh tái phát lỗi

- **Tận dụng tín hiệu firmware đã có.** Bộ phát đã báo tiến độ theo `playback_id` và vị trí. Callback hiện chạy ngay trước bước chọn đầu ra và ghi PCM. Dùng nó để xác định robot đã tới tầng âm thanh, ghi thêm kết quả ghi PCM đầu tiên nếu driver cung cấp. Âm lượng 0, sai đầu ra và lỗi loa vật lý cần được phân biệt qua trạng thái hoặc kiểm tra nghe thật. [AudioService](../main/audio/audio_service.cc) · [NotifyPlayer](../main/notify/notify_player.cc)
- **Khép được lượt ngay cả khi Cloud gián đoạn.** Không giả định AI luôn chuyển nguyên mã lượt. Kiểm tra bằng log rằng mã đi được từ tool chuẩn bị tới tool phát. Nếu Cloud chỉ chuyển tiếp URL, dùng định danh lượt an toàn đi kèm URL phát có sẵn và ánh xạ tại server. Trước khi có tương quan chắc chắn, thiết bị chỉ đánh dấu sự kiện nó tự quan sát. Giai đoạn đầu có thể đối soát log local, chưa cần xây dịch vụ mới.
- **Đặt quy tắc ưu tiên âm thanh rõ ràng.** Chuông đến hạn khi đang nghe nhạc phải hiện lý do trên LCD. Hai chuông liên tiếp phải giữ đúng nhãn. Cho phép xác nhận chuông bằng thao tác tại chỗ khi Cloud mất. Giọng AI tôn trọng trạng thái đang báo, mỗi kết quả lỗi chỉ có một bên phát lời. Không áp dụng chặn câu trùng xuyên qua hai yêu cầu độc lập của người dùng.
- **Quyền truy cập khi chuyển VPS.** Dùng một endpoint cho mỗi hồ sơ cần tách ghi chú, tri thức và nhạc. Kiểm tra bản sao SQLite có thể phục hồi. README ghi nhận một token từng nằm trong source, cần đối chiếu việc thu hồi token cũ trước khi mở dịch vụ cho người khác. Trạng thái thu hồi chưa được xác nhận trong đợt rà này. Tài liệu Studio cũng còn cả mô tả thử nghiệm REX2 và luồng REX3 đang dùng, nên cập nhật cùng bản phát hành để quản trị viên chọn đúng quy trình. [Hướng dẫn server](../mcp_music_server/README.md) · [Studio](../mcp_music_server/expression_studio.py)

## 8. Đối chiếu kiểm thử

| Phép kiểm tra | Kết quả |
|---|---|
| Phụ thuộc của Python 3.13 đúng tiến trình đang phục vụ Fleet | `py -3.13 -m pip check` không phát hiện phụ thuộc hỏng. FFmpeg trên PATH báo phiên bản 8.0.1 và liệt kê encoder `libopus`. |
| Backend trong cây mã hiện tại | `py -3.13 -X utf8 -m unittest discover -s mcp_music_server` đạt 44/44. Có kiểm tra URL theo card mạng, ticket hết hạn/thay thế, lỗi FFmpeg trước HTTP 200, khử trùng RPC, cách ly hồ sơ và trạng thái Fleet khi nhiều kết nối. |
| Host tests firmware chạy lại trên Windows | `py -3.13 -X utf8 -m unittest discover -s scripts/tests` ghi nhận 149 bài, 13 lỗi và 28 bỏ qua. Năm lỗi khi dọn thư mục tạm đang bị Windows giữ, sáu lỗi khi gọi Bash, hai lỗi khi khởi chạy trình biên dịch cho test NotifyPlayer/Ogg. Đây là khoảng trống xác minh cục bộ, chưa có căn cứ từ các lỗi này để kết luận robot phát âm thanh sai. |
| Firmware trên CI | Đã đối chiếu commit `6e6c06d`. Job host tests và sáu job build đều thành công, gồm LCD 240×240 thường, KCX và 240×320. [Lần chạy CI](https://github.com/LTuanpn/xiaozhi-esp32/actions/runs/44722718564) |
| Phần cứng và dịch vụ ngoài | Đợt rà này đọc trạng thái Fleet và log phục vụ thật. Các lần thử bằng giọng, QR Redmi và nạp firmware ghi ở mục 2 là kết quả từ các lần làm việc trước. Chưa đo tỷ lệ thành công, độ trễ đầu ra loa, độ đúng khi hai báo động trùng nhau, khả năng phục hồi sau mất điện hoặc tải nhiều robot trên VPS. |

## 9. Bàn giao bản nâng cấp để thử trên robot

**Firmware 2.5.20 và Music MCP 2.3.0 đã sẵn sàng để thử trên thiết bị.** Cấu hình vai trò Jarvis đã được lưu vào Xiaozhi Cloud và đọc lại thành công. Bản firmware vẫn nằm trên máy phát triển. Tiến trình phục vụ cổng 8765/8766 khi bàn giao vẫn là bản 2.2.6. Mã khởi chạy và gói phát hành trên đĩa là bản 2.3.0. Lớp duyệt lệnh của môi trường đã chặn thao tác dừng tiến trình cũ để chuyển phiên. Bản 2.3.0 được khởi động và xác minh riêng trên loopback. Không có lệnh nạp, phát âm thanh hoặc kích hoạt báo động gửi tới robot trong đợt triển khai này.

| Phần đã nâng cấp | Hành vi có trong bản mới |
|---|---|
| Hỏi và thao tác trên thiết bị | Trạng thái chung trả số giây, nhãn, timer đang đếm/chờ báo/đang reo, thời gian tập đã qua/còn lại, GUARD, loại chuông và hoạt động âm nhạc. URL OTA quản trị vẫn chỉ nằm trong trạng thái LAN đã ghép đôi. Gia hạn timer giữ nguyên deadline gốc cộng số giây, yêu cầu thay timer cần xác nhận. |
| Alarm và điều khiển tại chỗ | Từng nguồn giữ deadline riêng. Alarm bận thì sự kiện kế tiếp vẫn chờ. Chọn sự kiện đến hạn sớm nhất, rồi mới xóa khi bộ phát đã nhận. BOOT và nút LAN tắt chuông. Workout hiển thị tiến độ và bíp theo khoảng 10 giây khi đường tiếng đang rảnh. |
| Gọi nhạc | Mã lượt liên kết chuẩn bị, tìm nguồn, xử lý kết quả và đầu ra. AI có thể chuyển mã qua tham số hoặc URL khi server đã nhận được mã. Hủy, hết hạn và yêu cầu mới vô hiệu hóa trả lời phát trễ có mã cũ. Kết quả mơ hồ/lỗi có bước báo kết thúc cho robot. Cache trên server dùng lại kết quả nếu Cloud gửi lại cùng lượt có mã. Firmware cũ tiếp tục dùng đường tương thích và không có mức đối chiếu này. |
| Hiển thị và tra lỗi | Robot hiện đang mở luồng, chỉ chọn mặt nhạc sau khi tác vụ âm thanh chuyển PCM đến codec. Serial có thời gian prepare_to_pcm_ms. LCD phân biệt mở lỗi với ngắt giữa bài. Dashboard tách trạng thái gửi byte ở server và trạng thái robot báo về qua Fleet. Lần báo có thể trễ khoảng 15 giây. Hai lỗi phát local vẫn dùng chung clip âm thanh cảnh báo. |
| Fleet và VPS | Có ghép UUID robot với phiên MCP khi nhiều kết nối. Có trạng thái đầu ra theo mã lượt, thời điểm quan sát, kiểm tra FFmpeg/libopus lúc khởi động, mẫu systemd và Caddy cho VPS. QR Wi-Fi giữ SSID và portal gốc, dòng dưới hướng dẫn chọn SSID thủ công và BOOT để thoát. |

### Kiểm chứng trước khi bàn giao

- CI đúng commit `a86ab558804f5b9e97961db85b2e82af1b6ca830` đạt host tests và sáu biến thể firmware. Binary LCD 240×240 thường có 3.007.008 byte, ESP32-S3, ESP-IDF v6.1.1-3-g60f3405f259. Esptool xác nhận checksum và validation hash hợp lệ. SHA-256 của **toàn bộ file** là `620bf08c959969961ab598d511253058f32569a4d644cc07d6758c52c0237038`. [CI bản bàn giao](https://github.com/LTuanpn/xiaozhi-esp32/actions/runs/44793876955)
- Backend hiện tại đạt **47/47** kiểm thử trên Windows, giao diện Fleet build thành công. Hai lượt thử nguồn thật bằng HTTP client trên máy tính lấy đúng video Tò Te Tí và Chúng Ta Của Hiện Tại. Đọc được 8192 byte Ogg Opus đầu sau **3,28 giây** và **3,00 giây** tính từ lúc client bắt đầu yêu cầu. Hai phép đo này chưa bao gồm thời gian hiểu câu nói của AI, Wi-Fi robot và loa.
- Đã giải nén gói server, chạy bằng dữ liệu sạch và cổng riêng, xác nhận API 2.3.0 có 23 tool, trang Fleet, hai gói giao diện gốc/mint, các bộ biểu cảm gốc và preview RoboEyes tích hợp. Bản sao SQLite của Fleet và chỉ mục tri thức đã mở lại và kiểm tra toàn vẹn. Gói phân phối loại khóa, hồ sơ cá nhân, SQLite và log vận hành. SHA-256 gói ZIP là `8232fb13deadc8697488066aec9fd86f2da66869ff4da257282e517e62a77991`.

### Thứ tự thử ngày mai

1. Chuyển phiên Music MCP đang phục vụ sang 2.3.0, kiểm tra API Fleet báo đúng version và phiên MCP connected. Nạp **ảnh ứng dụng** 2.5.20 đúng biến thể, bảo toàn NVS và xác nhận phiên bản trên thiết bị.
2. Đặt timer 45 giây, hỏi còn bao lâu, thêm 30 giây và hỏi lại. Tạo một timer và một workout gần cùng thời điểm, xác nhận từng chuông bằng BOOT. Kiểm tra nhãn, thứ tự và nhiệm vụ còn lại cả khi ngắt Internet sau khi đặt.
3. Nói mỗi bài từng lỗi một lần. Đối chiếu log tìm nguồn, HTTP, dòng PCM và âm thanh nghe thấy. Thử dừng khi đang tìm, đổi bài, giả lập mất MCP, khôi phục và kiểm tra chỉ có một thông báo cuối.
4. Mở QR điều khiển, đóng, mở QR Wi-Fi gốc, thử Redmi và trở về Wi-Fi đã lưu. Thử chuông với RoboEyes tích hợp và gói OTA. Xác minh wake word và ngắt lời khi phát nhạc.
5. Khi có VPS và tên miền, chạy thử qua HTTPS từ Wi-Fi khác, kiểm tra khởi động lại và khôi phục. Mục tiêu 95% và điểm 9/10 vẫn cần bộ mẫu giọng thật. Hẹn giờ qua mất điện, nhiều timer có tên, nghe tiếp đúng vị trí và hiểu tiếng Việt offline là các tính năng kế tiếp.
