# Bread Compact Wi-Fi LCD 240×240 – mic SCK 4

Chọn biến thể `bread-compact-wifi-lcd-240x240-mic-sck4` cho ESP32-S3 LCD ST7789 240×240 dùng I²S simplex với SCK của mic đã hàn vào GPIO4 và WS vào GPIO5. DIN của mic là GPIO6. Loa MAX98357A vẫn dùng DOUT GPIO7, BCLK GPIO15 và LRCK GPIO16.

```powershell
python scripts/build.py bread-compact-wifi-lcd --name bread-compact-wifi-lcd-240x240-mic-sck4 --wake-word wn9_jarvis_tts
```

Biến thể báo tên riêng với Fleet để OTA phân biệt đúng sơ đồ dây. Khi chuyển từ bản LCD 240×240 chuẩn, xác nhận sơ đồ phân vùng trước rồi nạp ảnh ứng dụng qua USB để giữ NVS và assets. Sau khi khởi động, đọc tên board qua USB, chờ OTA báo lên Fleet và kiểm tra đầu vào mic bằng lệnh gọi Jarvis.
