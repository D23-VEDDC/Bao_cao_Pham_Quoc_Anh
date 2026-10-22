VEDC STM32 - RETRY UART THEO THOI GIAN XU LY THUC TE
=====================================================

LEARN
-----
1. CMD_LEARN_START: retry moi 3 giay neu chua nhan ACK.
2. CMD_CAPTURE_SAMPLE (ngoai quan): retry moi 3 phut 20 giay (200000 ms).
   Ly do: ESP32-S3 co the mat khoang 3 phut de chup/xu ly/trich feature 1 anh.
3. CMD_POWER_SAMPLE (dong dien): retry moi 5 giay neu chua nhan POWER_ACK.
4. CMD_TRAIN_START: retry moi 15 giay neu chua nhan TRAIN_DONE.

CHECK
-----
1. CMD_CHECK_START: retry moi 3 giay neu chua nhan ACK.
2. CMD_VISUAL_CHECK (ngoai quan): retry moi 3 phut 20 giay (200000 ms).
3. CMD_POWER_CHECK (dong dien): retry moi 5 giay neu chua nhan POWER_RESULT.
4. CMD_CHECK_NEXT: retry moi 3 giay neu chua nhan ACK.

IMPORTANT
---------
- Day la KHOANG CACH GIUA HAI LAN GUI LAI request, khong phai timeout ket thuc transaction.
- STM32 van giu nguyen state va request cu cho den khi nhan duoc phan hoi hop le.
- Retry ngoai quan dung uint32_t vi 200000 ms vuot gioi han uint16_t 65535 ms.
- Retry power KHONG do INA219 lai; STM32 gui lai payload Imax/Iavg da cache.
- Retry duplicate phai duoc ESP32-S3 xu ly idempotent/replay response nhu ban reliable da sua.
