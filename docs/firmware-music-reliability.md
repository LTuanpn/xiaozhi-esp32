# ESP32-S3 TFT 240×240: reliability changes

Target: `bread-compact-wifi-lcd-240x240`, ESP-IDF release/v6.1, build with `--wake-word wn9_jarvis_tts`. GPIO and OTA board identity are unchanged. Optional lamp tooling is off by default (`BREAD_LCD_LAMP_ENABLED`).

Timer and GUARD use embedded Vietnamese Ogg assets through the bounded notification producer. Timer says “Hết giờ rồi. Đến giờ nghỉ tay nhé.”; GUARD says “Cảnh báo. Phát hiện tiếng động bất thường. Hãy kiểm tra xung quanh.” The voice was generated with vi-VN-NamMinhNeural and combined with synthetic tones using FFmpeg: mono Opus, 16 kHz, 20 ms frames. Custom timer labels appear on screen; they are not dynamically spoken. GUARD is a one-shot sound-level detector, with arming calibration and suppression while the robot plays audio.

Ending a conversation sends stop-listening before changing state. Late TTS start/stop is suppressed until the next wake/listening request. Notification cancellation is separate from worker cleanup, and audio generation checks reject old packets after decoder reset. One worker keeps only the latest pending playback. A stalled HTTP operation can still delay a replacement notification until its 5-second timeout; this needs hardware timing validation. Wake listening during playback requires the AFE wake-word path used by this S3 build.

Manual emotion requests exit QR/standby and hold their selected face for a bounded time. LVGL owns the frame timer, removing display-lock waits from the shared ESP timer task. Countdown text uses monotonic elapsed time. QR uses integer RGB565 modules, a quiet zone of at least four modules, automatic mask selection and versions up to 9 at 192×192 pixels. Oversized payloads fail explicitly. Native QR content is prepared outside the display lock.

`main/boards/common/qrcodegen.c` and `.h` are restored from Nayuki QR Code generator, revision `bcfe1b018cec5f1f91b5705eca5bb4cdd34e9f8d` (MIT license retained). Source: https://github.com/nayuki/QR-Code-generator/tree/bcfe1b018cec5f1f91b5705eca5bb4cdd34e9f8d/c . Host QR tests compile the shipped encoder and decode rendered pixels with zxing-cpp, including version/interleaving boundaries and escaped Wi-Fi payloads.

## Acceptance on the robot

1. Record board identity/build SHA, flash/PSRAM size and wake model from boot. Back up the actual partition table and NVS before flashing; derive offsets from that table, never assume an NVS offset.
2. Timer: expiry offline produces tone and speech; cancel/re-arm produces no stale alarm. Trigger during music, then interrupt with wake/button. Measure alarm delay with an unreachable/stalled stream.
3. GUARD: after arming, ambient noise is quiet; claps trigger once. Music and the robot's own voice must not trigger it. This is not an intrusion detector.
4. Emotion: all prompt aliases; invalid name; requests from standby and QR; hold duration while LLM/status arrives; return to normal UI.
5. QR: scan URL, Wi-Fi with punctuation and long payload using a real phone at normal brightness. Repeat QR/standby/music while checking wake latency, heap and audio underruns.
6. Audio: capture, playback, wake/VAD, interruption, reconnect, WebSocket and MQTT/UDP; test configured AEC separately. Trace one user request through STT/tool/TTS to diagnose duplicate speech.
7. Music: exact title/artist, ambiguous request, no match, stop during search/startup, consecutive songs, disconnect/reconnect. Record video ID/time for any unwanted audio; do not claim universal ad removal.

Host tests and successful CI builds cannot establish that these physical checks passed. Cloud prompt changes are supplied separately in `mcp_music_server/xiaozhi-cloud-rules.md`.
