#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Script kiểm thử tự động toàn bộ 23 biểu cảm Wall-E & Huy Vector RoboEyes qua Serial Monitor COM port.
Sử dụng: python test_emotions.py [PORT] [BAUD] [DELAY_SECONDS]
Ví dụ: python test_emotions.py COM5 115200 2.5
"""

import sys
import time
import serial

# Thiết lập UTF-8 cho console Windows
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
DELAY = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0

EMOTIONS = [
    # 1. Nhóm 12 Biểu cảm Nền tảng
    ("neutral", "Mat Wall-E goc - Liec mat to mo tu dong"),
    ("happy", "Mim cuoi nhe nhang - Mi duoi nang nhe"),
    ("laugh", "Cuoi tit mat cuc vui - Nhun nhay nhip nhang"),
    ("wink", "Nhay mat tinh nghich 1 ben"),
    ("sad", "Buon ba ru mi xuong doi xung"),
    ("cry", "Khoc rung rung nghen ngao + Nuoc mat roi 2 ben"),
    ("angry", "Tuc gian cau may 14 do doi xung chuan Huy Vector"),
    ("surprised", "Soc ngac nhien: Dong tu co rut 720ms roi gian no cuc dai"),
    ("sleepy", "Lo do buon ngu: Sup 1/3 mi tren, mi duoi phang, liec mat tu nhien"),
    ("sleeping", "Ngu say thu thai: Hai vom cong trang khuyet tho em"),
    ("thinking", "Suy nghi nghiem tuc: Liec mat goc tren phai"),
    ("shy", "Xau ho do mat: 2 vet ma hong e then"),

    # 2. Nhóm 11 Biểu cảm Động lực học V6.0
    ("skeptical", "Hoai nghi / To mo: Mat trai to tron, mat phai nheo nghieng 6 do"),
    ("smug", "Dac y / Cuoi khey: Mi duoi nhech 14px + Liec len goc tren + Gat gu"),
    ("furious", "Cuc ky thinh no: Cau may 18 do + Run ban bat + 4 luong khoi boc len"),
    ("love", "Trai tim phap phong: 2 trai tim dap thin thich + Ma hong"),
    ("dizzy", "Chong mat / Say: 2 dau X xoay tron 360 do + Chao dao elip"),
    ("scared", "Hoang so: Dong tu co nho + Run ban bat + Giot mo hoi roi ben thai duong"),
    ("excited", "Hao hung: Mat ngoi sao 4 canh lap lanh co gian + Nhun nhay"),
    ("bored", "Chan chuong: Sup 50% mi + Dao mat uoi oai trai -> phai -> len tran"),
    ("pout", "Phung phinh: Dong tu cuc dai anime + 2 dom sang lung linh"),
    ("music", "Thuong thuc am nhac: Mat cuoi chill + Du dua trai phai + 2 not nhac bay"),
    ("scanning", "Quet radar AI: Mat cong nghe + Tia laser ngang quet lien tuc"),

    # 3. Nhóm 6 Biểu cảm Cyber & Tech
    ("matrix", "Matrix glitch rach hinh ngang lien tuc"),
    ("loading", "Spinner loading xoay tron cho tai du lieu"),
    ("battery", "Yeu pin sup mi + Icon pin chot tat"),
    ("overheat", "Qua nhiet nong may + 4 song khoi boc len"),
    ("reboot", "Khoi dong CRT vach sang mo rong"),
    ("target", "Hong tam ban tia khoa muc tieu"),

    # 4. Nhóm 7 Biểu cảm Personality & Sass
    ("side_eye", "Bombastic side-eye liec xeo phan xet"),
    ("facepalm", "Bat luc can loi 1 mat che kin"),
    ("innocent", "Ngay tho vo toi + Vong hao quang thien than"),
    ("jealous", "Ghen ti nheo mi tam giac xeo"),
    ("scheming", "Gian xao muu mo liec qua liec lai"),
    ("disgust", "Kinh tom nhan nho lech mat"),
    ("whisper", "Thi tham bi mat 2 mat nep sat mep"),

    # 5. Nhóm 6 Biểu cảm Physical & Bodily
    ("freeze", "Lanh run cam cap rang cua va lap cap"),
    ("sneeze", "Sap hat xi co rum roi bung gio"),
    ("waterfall", "Thac nuoc Anime tuon xoi xa 2 ben"),
    ("headache", "Dau dau nhuc oc + 2 tia set giat thai duong"),
    ("drunk", "Say xin 1 mat to 1 mat be lao dao"),
    ("exhaust", "Kiet suc nam bep sat day man hinh"),

    # 6. Nhóm 6 Biểu cảm Mental & Spiritual
    ("zen", "Thien dinh thanh tinh bong benh"),
    ("jawdrop", "Rot ham bang hoang chan dong"),
    ("paranoid", "Hoang tuong dao mat loan xa canh giac"),
    ("question", "Ngo ngac dau hoi ? nhay tung tung"),
    ("idea", "Nay y tuong bong den sang loa"),
    ("hypno", "Thoi mien vong xoay dong tam"),

    # 7. Nhóm 6 Biểu cảm Playful & Gaming
    ("money", "Mat sang hinh do la $"),
    ("heartbreak", "Trai tim vo doi vo vun"),
    ("tongue", "Le luoi treu nguoi tinh nghich"),
    ("party", "Tiec tung an mung hoa giay confetti"),
    ("stealth", "Ninja an nap he he mat trong dem"),
    ("game_over", "Thua tran dau X X chim xuong day"),

    # 8. Nhóm 6 Biểu cảm Elemental & Powers
    ("fire", "Mat lua Super Saiyan bung chay"),
    ("thunder", "Te liet vi set danh giat ziczac"),
    ("rain", "Am dam may den mua roi ti tach"),
    ("sunny", "Choi chang toa sang tia nang"),
    ("sleepwalk", "Mong du do dan vo hon vat vo"),
    ("angel", "Thanh thien ban phuoc hao quang doi canh"),
]

def main():
    print(f"=== KHỞI ĐỘNG BỘ TEST ROBOEYES TRÊN CỔNG {PORT} (BAUD {BAUD}) ===")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
    except Exception as e:
        print(f"[LỖI] Không thể kết nối tới {PORT}: {e}")
        print("Vui lòng kiểm tra lại cáp kết nối hoặc đóng các phần mềm đang chiếm dụng cổng COM.")
        sys.exit(1)

    time.sleep(2)
    print(f"Bắt đầu kiểm tra {len(EMOTIONS)} biểu cảm (mỗi biểu cảm {DELAY}s)...\n")

    for idx, (emo, desc) in enumerate(EMOTIONS, 1):
        cmd = f"face {emo}\n"
        print(f"[{idx:02d}/{len(EMOTIONS):02d}] Gửi lệnh: '{cmd.strip()}' -> {desc}")
        ser.write(cmd.encode("utf-8"))
        ser.flush()

        start = time.time()
        while time.time() - start < DELAY:
            if ser.in_waiting:
                line = ser.readline().decode("utf-8", errors="ignore").strip()
                if "Serial CLI command" in line or "SetEmotion" in line:
                    print(f"     [Robot Log] {line}")
            time.sleep(0.05)

    print("\n=== HOÀN TẤT KIỂM THỬ TOÀN BỘ BIỂU CẢM THÀNH CÔNG ===")
    ser.close()

if __name__ == "__main__":
    main()
