# So sanh vector TFLite PC va ESP32

Model mac dinh: `main/pcb_embedding_64_i8in_f32out.tflite`.

Khong co `work/COLAB_INT8_CELL.py` hay anh webcam hieu chuan trong project.
`tools/COLAB_INT8_64_CELL.py` la cell cu, chua mo phong buoc tim/chuan hoa PCB.
De doi chieu model voi firmware moi, lay BMP `/api/normalized-image`, cat patch
64x64 stride 48 tu anh nay. Can model Keras goc va frame webcam that de xuat lai
TFLite; model hien tai chua duoc hieu chuan voi patch da chuan hoa.

1. Trong menuconfig, bat `VEDC AI Debug -> Dump one patch and its raw/L2 embedding vectors to serial` va chon patch index (0..116). Frame 640x480 tao 117 patch; kich thuoc frame khac bi tu choi.
2. Lay 12,288 byte RGB888 trong log `[PATCH DUMP]`, luu thanh `patch.rgb`. Luu 32 gia tri `l2=` tu cac dong `[EMBED VECTOR]` vao mang JSON trong `esp-l2.json`; tuy chon luu cac gia tri `raw=` thanh `esp-raw.json`.
3. Chay:

```text
python tools/validate_vectors.py --patch-rgb patch.rgb --esp-vector esp-l2.json --esp-raw esp-raw.json
```

Script in metadata model, raw output PC, norm, vector L2, cosine va sai so toi da.
No chi doi chieu vector, khong danh gia nguong PatchCore hay do chinh xac phat hien loi.
