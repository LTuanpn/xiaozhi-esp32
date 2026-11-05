---
name: walle-roboeyes-designer
description: >-
  Chuyên gia thiết kế biểu cảm mắt robot sống động phong cách Wall-E / Huy Vector / Pixar
  cho màn hình OLED SSD1306 (128x64) và màn hình LCD/TFT trên ESP32 sử dụng LVGL 9.
  Cung cấp quy chuẩn hình học, thuật toán sửa lỗi mí mắt xoay góc âm, danh mục 60 biểu cảm
  động lực học phong phú, đồng bộ ngữ cảnh giọng nói LLM và kiểm thử tự động.
---

# Wall-E & Huy Vector RoboEyes Designer Skill

Kỹ năng này đóng gói toàn bộ quy chuẩn thiết kế, thuật toán hình học và kinh nghiệm thực chiến để tạo ra đôi mắt robot giàu cảm xúc, sinh động theo phong cách **Wall-E / Huy Vector / Pixar** trên màn hình OLED SSD1306 (128x64) hoặc màn hình LCD/TFT (GC9A01, ST7789) chạy trên nền tảng **ESP32** với thư viện đồ họa **LVGL 9**.

---

## 1. Triết Lý Thiết Kế & Các Bài Học Xương Máu (Lessons Learned)

Khi thiết kế mắt robot, đặc biệt là trên màn hình đơn sắc OLED 128x64, **tuyệt đối tuân thủ các nguyên tắc sau**:

### ❌ Những Sai Lầm Phải Tránh (Không Được Lặp Lại)
1. **Không dùng mắt tròn xoe hay hình elip đơn giản**: Mắt robot hiện đại cần nét vuông vức nhưng bo góc mềm mại như Vector của Anki / Wall-E. Tỷ lệ chuẩn trên màn hình 128x64 là `44x36px` với bo góc `radius = 8`.
2. **Không vẽ đốm sáng tĩnh (Catchlight) màu đen**: Trên màn hình OLED đơn sắc (mắt trắng nền đen), nếu vẽ một đốm sáng tĩnh màu đen bên trong đồng tử thì khi robot liếc mắt qua lại, đốm đen đứng yên sẽ tạo cảm giác như **mắt bị lé hoặc đứng đờ ra một chỗ**. Tròng mắt phải là màu đen thuần túy hoặc đốm sáng chuyển động đồng bộ.
3. **Không híp mắt thành 2 đường cong giật giật khi Cười (`laugh`)**: Việc híp mắt hoàn toàn lại rồi giật lên giật xuống trông giống hệt như đang ngủ bị co giật. Biểu cảm cười chuẩn phải là **mắt mở tươi vui, mí dưới nâng nhẹ (5-8px) hình vòm má cười và toàn bộ mắt nhún nhảy tưng bừng**.
4. **Không co nhỏ 2 mí lại khi Buồn ngủ (`sleepy`)**: Buồn ngủ phải thể hiện đúng chứng **sụp mí trên (ptosis)**: *Chỉ có mí trên hạ xuống 1/3 (12px), mí dưới giữ phẳng hoàn toàn, và tròng mắt vẫn liếc trái phải tự nhiên*.
5. **Không làm Ngạc nhiên (`surprised`) đơ một chỗ**: Ngạc nhiên phải có phản xạ sinh học: **Đồng tử co rút cực nhỏ (8x10px) trong 720ms đầu, sau đó mới phóng to giãn nở cực đại (24x26px) kèm rung chấn động (shock vibration)**.
6. **Không để Khóc (`cry`) bị lệch hay thiếu nước mắt**: Cần có 2 giọt nước mắt rơi đều đặn dưới cả 2 mắt, hai mí trên rủ xuống đối xứng góc -14° và +14°, toàn bộ mắt rung nhẹ nghẹn ngào.
7. **Không được để lại vệt chém khi Cau mày (`angry` / `furious`)**: Khi xoay mí mắt để cau mày, phải dùng thuật toán xoay chuẩn tâm. Tuyệt đối không để mí mắt bị cắt xéo ngang qua con mắt như một vệt chém dao găm.
8. **Áp dụng Squash & Stretch (Nguyên lý Disney)**: Khi mắt dẹt xuống (chớp mắt, sụp mí), chiều rộng mắt phải nở ngang ra (`44 -> 48-50px`). Khi mắt mở to ngạc nhiên, chiều ngang co nhẹ lại (`42-40px`), tạo cảm giác dẻo mềm và sinh động.
9. **Tối ưu hóa đồ họa 1-bit Monochrome**: Tránh vẽ các hình khối đặc (`bg_opa = LV_OPA_COVER`) vì trên màn OLED đen trắng chúng sẽ biến thành cục gạch thô ráp. Hãy dùng viền rỗng (`bg_opa = LV_OPA_TRANSP, border_width = 2`) cho vòng hào quang Halo, khung pin cạn, miệng chữ O. Dùng `lv_label` với font montserrat cho các ký hiệu sắc nét (`?`, `!`, `$`, `LV_SYMBOL_AUDIO`, `LV_SYMBOL_CHARGE`).
10. **Quy chuẩn Z-Index Layering**: Tạo đối tượng theo đúng thứ tự: Nền `face_layer_` -> Mắt trắng -> Đồng tử đen & Dizzy X/Sparkle trong mắt -> Mí mắt che chắn -> Nước mắt & Khói & Nhãn FX trên cùng.

---

## 2. Thuật Toán Hình Học Cốt Tử Trong LVGL 9

### A. Hàm Chuẩn Hóa Góc Xoay `NormalizeAngle()` (Bắt Buộc Phải Có)
Trong LVGL 9, kiểu dữ liệu góc xoay là `uint16_t` (đơn vị 0.1 độ, từ 0 đến 3600). Nếu truyền trực tiếp góc âm (ví dụ `-140` cho -14 độ), biến sẽ bị **tràn số âm thành 65396**, khiến LVGL không thể render và mí mắt sẽ biến mất hoàn toàn!
Luôn sử dụng hàm chuẩn hóa sau:
```cpp
static inline int32_t NormalizeAngle(int32_t angle) {
    angle = angle % 3600;
    if (angle < 0) {
        angle += 3600;
    }
    return angle;
}
```

### B. Thuật Toán Đặt Tâm Xoay Mí Mắt (Transform Pivot)
Để mí mắt xoay chuẩn mà không bị lệch tâm hay biến dạng:
```cpp
lv_obj_set_style_transform_pivot_x(lid, lid_width / 2, 0);
lv_obj_set_style_transform_pivot_y(lid, lid_height / 2, 0);
lv_obj_set_style_transform_rotation(lid, NormalizeAngle(angle_deg * 10), 0);
```

### C. Chuyển Động Mượt Mà Bằng Hàm `Ease()`
Mọi sự thay đổi về toạ độ liếc nhìn (`target_look_x`, `target_look_y`) hoặc chiều cao mắt (`target_height`) đều phải đi qua bộ lọc làm mượt (tần số 25 FPS, timer 40ms):
```cpp
int Ease(int current, int target, int rate = 3) {
    if (current < target) return std::min(target, current + rate);
    if (current > target) return std::max(target, current - rate);
    return current;
}
```

### D. Công Thức Vẽ Trái Tim Hoạt Hình Bằng 2 Hình Chữ Nhật Bo Góc
Không cần dùng Canvas phức tạp gây tốn RAM. Tạo 2 hình chữ nhật bo góc `w = 12px, h = 22px, radius = 6`, cùng đặt tại tâm mắt và xoay giao nhau góc 90°:
- Nhánh A: xoay `NormalizeAngle(450)` (+45°)
- Nhánh B: xoay `NormalizeAngle(-450)` (-45°)
- Đập phập phồng (pulse nhịp tim): `w = 12 + pulse; h = 22 + pulse * 2;` với `pulse = (tick % 14 < 7) ? 2 : -1`.

---

## 3. Bảng Danh Mục 60 Biểu Cảm Động Học Hoàn Chỉnh

Dưới đây là danh mục 60 biểu cảm với thông số hình học và animation động lực học chi tiết (25 FPS), hiển thị sống động trên màn hình OLED SSD1306 (128x64):

### Nhóm Gốc & Kinh Điển (23 Biểu Cảm Cốt Lõi)
| Tên Biểu Cảm | Mắt Trái / Phải | Mí Mắt & Phụ Kiện | Động Lực Học Animation (25 FPS) |
| :--- | :--- | :--- | :--- |
| `neutral` | 36 x 44 | Tự nhiên, không mí | Tự động liếc nhìn tò mò sau mỗi 2-4s, chớp mắt tự nhiên. |
| `happy` | 36 x 44 | Mí dưới che 14px vòm cười | Ánh mắt mỉm cười dịu dàng, nhìn thẳng. |
| `laugh` | 36 x 44 | Mí dưới che 8px vòm má | Mắt tươi vui, nhún nhảy `animation_y = -2 <-> 1` theo nhịp cười. |
| `wink` | 36 x 44 / 22 x 44 | Mắt phải mặt nạ trăng khuyết | Mắt phải nháy tinh nghịch, mắt trái mở to. |
| `sad` | 32 x 44 | Mí trên rủ -14° / +14° | Mắt trĩu xuống, tròng mắt nhìn xuống (`target_look_y = 3`). |
| `cry` | 32 x 44 | Mí rủ -14° / +14° | Rung nghẹn ngào + 2 giọt nước mắt rơi luân phiên dưới 2 mắt. |
| `angry` | 36 x 44 | Cau mày 14° (+140 / -140) | Mí trên cắt chéo đối xứng chuẩn Huy Vector, không vệt chém. |
| `surprised` | 42 x 44 | Mở to cực đại | Đồng tử co 8x10 trong 720ms rồi bung to 24x26 + rung chấn giật mình. |
| `sleepy` | 36 x 44 | Mí trên sụp 12px (1/3), mí dưới phẳng | Sụp mí trên lờ đờ, tròng mắt vẫn liếc ngang tự nhiên. |
| `sleeping` | 22 x 44 | Hai vòm trăng khuyết radius 14 | Nhắm mắt ngủ say, thở phập phồng êm ái (`breath_offset`). |
| `thinking` | 36 x 44 | Tự nhiên | Liếc góc trên bên phải (`x=7, y=-4`), rung nhẹ suy ngẫm. |
| `shy` | 32 x 44 | Tự nhiên | Liếc e thẹn góc dưới trái + 2 vệt má hồng phúng phính. |
| `skeptical` | 40 x 44 / 20 x 44 | Mắt phải nheo nghiêng -6° | Mắt trái mở to tò mò, mắt phải nheo hoài nghi liếc ngang. |
| `smug` | 36 x 44 / 34 x 44 | Mí dưới phải nhếch cao 14px | Nụ cười khẩy một bên + liếc góc trên phải + gật gù đắc ý. |
| `furious` | 36 x 44 | Cau mày 18° + mí dưới 6px | Rung bần bật tức tối + 4 luồng khói bốc lên đỉnh đầu. |
| `love` | Ẩn mắt thường | 2 trái tim đập phập phồng | 2 trái tim nhịp đập 14 <-> 18px + 2 má hồng rực rỡ. |
| `dizzy` | 36 x 44 | 2 dấu X đen quay tít | 2 dấu X xoay 360° liên tục + mắt chao đảo hình elip say sóng. |
| `scared` | 30 x 44 | Mở co tròn | Rung lắc bần bật khiếp sợ + giọt mồ hôi rơi bên thái dương. |
| `excited` | 38 x 44 | Tự nhiên | Đồng tử ngôi sao 4 cánh lấp lánh co giãn + nhún nhảy tưng bừng. |
| `bored` | 36 x 44 | Mí trên sụp ngang 50% (18px) | Mí sụp 1/2, đảo mắt uể oải trái -> phải -> trần nhà thở dài. |
| `pout` | 38 x 44 | Tự nhiên | 2 đốm sáng lung linh trong tròng đen + má hồng phúng phính. |
| `music` | 36 x 44 | Mí dưới nâng cao vòm cười | Đu đưa lắc lư sang 2 bên theo nhịp + 2 nốt nhạc bay dập dờn. |
| `scanning` | 26 x 44 | Mắt nheo công nghệ | Tia laser quét ngang từ đỉnh xuống đáy + đồng tử đảo nhanh. |

### Nhóm 1: Cyber & Công Nghệ (6 Biểu Cảm)
| Tên Biểu Cảm | Mô Tả & Tạo Hình | Animation Động Lực Học |
| :--- | :--- | :--- |
| `matrix_glitch` | Mắt lệch pha ngang dọc, thanh quét glitch CRT | Mắt rung giật mạnh, vệt quét scanline trượt qua lại. |
| `loading_buffer` | Đồng tử biến thành thanh loading 4 chấm | 4 vạch loading chạy xoay vòng nối tiếp nhấp nháy. |
| `low_battery` | Mắt chớp tắt lờ đờ, biểu tượng pin cạn góc màn hình | Mắt tối dần rồi nháy yếu ớt, nhấp nháy đỏ báo pin yếu. |
| `overheat` | Mắt rực nóng, sóng nhiệt bốc hơi | Mắt co giật vì quá nhiệt, sóng nhiệt dập dờn bay lên. |
| `rebooting` | Mắt thu nhỏ thành 1 đường chỉ sáng rồi nở bừng | Vạch sáng hẹp ngang nở rộng dần như TV bóng đèn CRT bật nguồn. |
| `target_locked` | Hồng tâm radar ngắm bắn thu phóng | Crosshair ngắm bắn co rút khóa mục tiêu liên tục. |

### Nhóm 2: Cá Tính & Thần Thái (7 Biểu Cảm)
| Tên Biểu Cảm | Mô Tả & Tạo Hình | Animation Động Lực Học |
| :--- | :--- | :--- |
| `side_eye` | Liếc xéo cực đại sang góc màn hình | Tròng mắt đẩy sát mép biên, mí mắt nheo phán xét. |
| `facepalm` | Một mắt mở hé, một mắt bị bàn tay/mí che | Một bên mắt giật giật bất lực, đầu lắc nhẹ sang bên. |
| `innocent` | Mắt tròn xoe long lanh ngây thơ | Đốm sáng lung linh, mắt chớp chớp vô tội. |
| `jealous` | Mắt híp một bên lườm nguýt ganh tị | Liếc mắt ghen ghét, mi mắt giật nhịp cay cú. |
| `scheming` | Mí trên mí dưới khép hẹp mưu mô | Ánh mắt liếc qua liếc lại xảo quyệt đầy toan tính. |
| `disgusted` | Mắt nheo lệch, mí nhăn nhó ghê tởm | Mắt giật lùi về sau, tròng mắt liếc tránh né kinh tởm. |
| `whispering` | Mắt đảo nhìn quanh dè chừng | Đảo mắt qua lại kiểm tra xung quanh thì thầm bí mật. |

### Nhóm 3: Phản Xạ Sinh Học & Cơ Thể (6 Biểu Cảm)
| Tên Biểu Cảm | Mô Tả & Tạo Hình | Animation Động Lực Học |
| :--- | :--- | :--- |
| `freezing` | Mắt co rúm, răng đánh lập cập | Rung lắc tần số cao vì rét buốt, bông tuyết/sương giá. |
| `sneezing` | Mắt nheo chặt co lại rồi nổ bung hắt xì | Mí khép chặt run run rồi giật bắn nảy bung cực mạnh. |
| `crying_stream` | 2 dòng thác nước mắt phun trào | Thác nước mắt đổ ào ạt 2 bên, cơ thể rung bần bật khóc lớn. |
| `headache` | Mắt nhắm nghiền nhăn nhó, tia sét nhói đau | Rung nhói theo nhịp mạch đập cơn đau đầu. |
| `drunk` | Hai mắt chao đảo lệch pha ngất ngưởng | Hai mắt quay vòng tròn elip so le, bước đi xiêu vẹo. |
| `exhausted` | Mắt rũ xuống đáy, lưỡi thè/giọt mồ hôi | Mắt sụp tận đáy, thở dốc phập phồng kiệt sức. |

### Nhóm 4: Tâm Trí & Nhận Thức (6 Biểu Cảm)
| Tên Biểu Cảm | Mô Tả & Tạo Hình | Animation Động Lực Học |
| :--- | :--- | :--- |
| `zen_meditation` | Mắt khép vòng cung tĩnh tại, hào quang | Trôi nổi êm đềm lơ lửng, vầng hào quang tỏa sáng dịu nhẹ. |
| `shocked_jawdrop` | Mắt co tròn trợn ngược cực sốc | Mắt thụt lùi rồi mở toáng cực đại đờ đẫn kinh hoàng. |
| `paranoid` | Hai mắt giật thon thót nhìn tứ phía | Đảo mắt cực nhanh tứ phía hoang tưởng, rung giật lo sợ. |
| `confused_question` | Một mắt to một mắt bé kèm dấu hỏi chấm | Dấu hỏi chấm bồng bềnh phía trên, đầu nghiêng thắc mắc. |
| `idea_eureka` | Mắt sáng bừng kèm bóng đèn bật sáng | Bóng đèn phát sáng trên đỉnh đầu nhấp nháy ý tưởng. |
| `hypnotized` | Đồng tử xoắn ốc thôi miên xoay tít | Vòng xoáy xoay tròn liên tục mê hoặc tâm trí. |

### Nhóm 5: Tinh Nghịch & Giải Trí (6 Biểu Cảm)
| Tên Biểu Cảm | Mô Tả & Tạo Hình | Animation Động Lực Học |
| :--- | :--- | :--- |
| `money_eyes` | Đồng tử biến thành ký hiệu dollar ($) | Ký hiệu dollar nhảy múa nhấp nháy lấp lánh tiền tài. |
| `heartbroken` | Trái tim vỡ đôi tách xa nhau | Trái tim nứt đôi rung nhẹ rồi tách rời trĩu xuống đau đớn. |
| `tongue_out` | Một mắt nháy, lưỡi thè tinh nghịch | Mắt nháy vui nhộn, lưỡi lúc lắc trêu chọc nhí nhảnh. |
| `party_celebrate` | Pháo hoa giấy confetti nổ tung | Mắt nhún nhảy tưng bừng, hạt hoa giấy bay lượn rộn ràng. |
| `stealth_ninja` | Mắt nheo hẹp sắc bén trong bóng tối | Mắt nheo sắc lẻm lướt nhanh trong bóng đêm bí ẩn. |
| `game_over` | Hai mắt biến thành dấu X tĩnh trĩu rủ | Mắt gục xuống, nhấp nháy báo hiệu thất bại GameOver. |

### Nhóm 6: Nguyên Tố & Huyền Ảo (6 Biểu Cảm)
| Tên Biểu Cảm | Mô Tả & Tạo Hình | Animation Động Lực Học |
| :--- | :--- | :--- |
| `fire_fury` | Ngọn lửa rực cháy quanh hốc mắt | Ngọn lửa bốc cháy bập bùng dữ dội bừng bừng nộ khí. |
| `thunder_shock` | Tia sét đánh chớp lóa qua mắt | Tia sét ziczac đánh giật sáng chói lóa điện giật. |
| `rainy_gloomy` | Đám mây u ám đổ mưa rơi rớt | Đám mây xám xịt phía trên trút từng hạt mưa rơi tí tách. |
| `sunny_dazzle` | Ánh mặt trời tỏa tia sáng rực rỡ | Vòng tia nắng quay tròn tỏa sáng rạng ngời ấm áp. |
| `sleepwalking` | Mắt mở đờ đẫn, tay buông thõng mộng du | Trôi dạt chậm rãi vô thức, ánh mắt vô hồn trôi nổi. |
| `angel_blessing` | Đôi cánh thiên thần vỗ nhẹ và vòng thánh | Vòng hào quang thiên thần bồng bềnh, cánh vỗ nhịp êm dịu. |

---

## 4. Cơ Chế Đồng Bộ Ngôn Ngữ & Giọng Nói LLM

### A. Nhận Diện Đa Ngôn Ngữ (Hàm `FaceModeFromEmotion`)
Bắt trọn vẹn các từ khóa tiếng Anh lẫn tiếng Việt mà Server LLM gửi về:
```cpp
PocketOledDisplay::FaceMode PocketOledDisplay::FaceModeFromEmotion(const char* emotion) const {
    if (emotion == nullptr) return FaceMode::kNeutral;
    if (Contains(emotion, "love") || Contains(emotion, "yeu") || Contains(emotion, "thich")) return FaceMode::kLove;
    if (Contains(emotion, "furious") || Contains(emotion, "thinh_no") || Contains(emotion, "dien_tiet")) return FaceMode::kFurious;
    if (Contains(emotion, "angry") || Contains(emotion, "tuc_gian") || Contains(emotion, "gian")) return FaceMode::kAngry;
    if (Contains(emotion, "smug") || Contains(emotion, "dac_y") || Contains(emotion, "kieu_ngao")) return FaceMode::kSmug;
    if (Contains(emotion, "skeptic") || Contains(emotion, "hoai_nghi") || Contains(emotion, "to_mo")) return FaceMode::kSkeptical;
    if (Contains(emotion, "cry") || Contains(emotion, "khoc")) return FaceMode::kSadCrying;
    if (Contains(emotion, "sad") || Contains(emotion, "buon")) return FaceMode::kSadEmpathy;
    if (Contains(emotion, "laugh") || Contains(emotion, "cuoi")) return FaceMode::kEcstaticLaugh;
    if (Contains(emotion, "scared") || Contains(emotion, "hoang_so") || Contains(emotion, "so")) return FaceMode::kScared;
    if (Contains(emotion, "music") || Contains(emotion, "nhac") || Contains(emotion, "hat")) return FaceMode::kMusic;
    if (Contains(emotion, "scan") || Contains(emotion, "quet")) return FaceMode::kScanning;
    if (Contains(emotion, "sleepy") || Contains(emotion, "buon_ngu")) return FaceMode::kSleepy;
    // ...
    return FaceMode::kNeutral;
}
```

### B. Khóa Biểu Cảm & Nhún Nhảy Khi Nói (`is_speaking_`)
Khi robot nói chuyện qua loa:
1. Trạng thái `SetStatus(SPEAKING)` bật cờ `is_speaking_ = true`.
2. Biểu cảm được khóa chặt ở `emotion_mode_` do LLM quyết định, không bị đè bởi trạng thái trung tính.
3. Trong suốt lúc nói, `animation_y = ((tick / 3) % 2 == 0) ? -1 : 1` tạo hiệu ứng nhấp nhô theo âm lượng giọng nói.
4. Hết câu nói, bộ đếm thời gian mới tự động đưa biểu cảm về trạng thái nghỉ (`kNeutral`).

---

## 5. Hướng Dẫn Nhân Bản Sang Các Màn Hình Khác

Nếu bạn làm việc với màn hình khác (như màn tròn GC9A01 240x240 hoặc màn hình màu ST7789 240x240 / 320x240):

1. **Hệ số Tỷ lệ (Scale Factor)**:
   - Trên OLED 128x64: Mắt `44x36px`, khoảng cách `12px`.
   - Trên màn hình 240x240: Nhân hệ số $\approx 1.8$, Mắt `80x68px`, bo góc `radius = 16`, khoảng cách `24px`.
   - Trên màn hình 320x240: Mắt `100x80px`, bo góc `radius = 20`.
2. **Khai thác Màu sắc trên màn hình Màu**:
   - Mắt bình thường: Màu xanh Cyan Neon (`lv_color_hex(0x00E5FF)`).
   - Tức giận / Furious: Mắt chuyển sang màu đỏ rực (`0xFF1744`), khói màu cam (`0xFF9800`).
   - Tình yêu: Trái tim màu hồng đậm (`0xE91E63`).
   - Âm nhạc: Nốt nhạc màu vàng rực (`0xFFEB3B`).
   - Scanning: Tia laser màu xanh lá dạ quang (`0x00E676`).
3. **Các file cần can thiệp**:
   - `main/boards/<board>/config.h`: Khai báo chân GPIO màn hình mới.
   - `main/boards/<board>/<board>_board.cc`: Khởi tạo display mới và đăng ký tool `self.screen.set_emotion`.

---

## 6. Hướng Dẫn Kiểm Thử Bằng Serial CLI

Trong file `<board>_board.cc`, luôn duy trì một FreeRTOS task đọc lệnh từ Serial:
```cpp
void InitializeSerialCli() {
    xTaskCreate([](void* arg) {
        auto* display = static_cast<Display*>(arg);
        char buf[64]; int idx = 0;
        while (true) {
            int c = getchar();
            if (c > 0 && (c == '\r' || c == '\n')) {
                buf[idx] = '\0';
                const char* cmd = (strncmp(buf, "face ", 5) == 0) ? buf + 5 : buf;
                display->SetEmotion(cmd);
                idx = 0;
            } else if (idx < 63) buf[idx++] = (char)c;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }, "serial_cli", 3072, display_, 1, nullptr);
}
```
Để test, chỉ cần mở terminal và chạy:
```bash
python scripts/test_emotions.py
```
Hoặc gõ trực tiếp qua bất kỳ phần mềm Serial Monitor nào (Baud 115200): `face love`, `face furious`, `face smug`, `face scanning`...
