# Log CAPTURE va operator TFLite

Firmware ghi ESP_LOGI ra console UART0 115200 va USB Serial/JTAG (neu board co
cong nay). UART1 noi STM32 la giao thuc, khong phai cong terminal log.
Sau khi flash, mo monitor dung cong ESP32-S3 va reset board de thay `[MODEL]`,
`[OP BACKEND]`, `[OP GRAPH]` va `[AI INIT] READY`.

Khi STM32 gui CAPTURE, terminal can thay theo thu tu:

1. `RX <- STM32: CMD=...`, `[CAPTURE] START` va UVC streaming.
2. Frame nhan, JPEG giai ma, tach nen va chuan hoa PCB, patch hash, `[CAPTURE] NORMALIZED PCB READY id=...`.
3. `[AI START]`, `[OP PROFILE]` cua patch duoc chon, `[AI PROGRESS]` va `[AI DONE]`.
4. `[CAPTURE] FEATURES READY` va `UART1 TX -> STM32: CMD=0x...`.

Web tai anh ngay sau buoc 2; score va heatmap chi co sau CHECK da hoan tat.
`/api/status` co `last_capture_id`; `/api/last-image?id=<id>` tra BMP PCB da chuan hoa cung
capture. Neu AI loi sau buoc 2, anh van duoc giu de xem nguyen nhan.

Menuconfig `VEDC AI Debug -> Log full model graph and per-operator timing`
bat mac dinh. `VEDC_AI_OP_PROFILE_INDEX` chon patch can do; log moi operator
co thu tu, ten, `elapsed_us`; `[OP OUTPUT]` ghi hash, min/max va so gia tri
khong huu han cua output tensor ngay sau moi operator. Profiler chi doc output
va do dong ho sau khi kernel xong, khong doi quantization, kernel hay vector.
Patch duoc chon mat them thoi gian quet tensor; `elapsed_us` chi tinh kernel.

De doi chieu voi TFLM reference, build firmware thu nghiem rieng voi
`VEDC_FORCE_TFLM_REFERENCE=y`, giu nguyen model va patch RGB. Log
`[OP BACKEND]` xac nhan mode. So sanh 32 raw output va vector L2 bang
`tools/validate_vectors.py` truoc khi ket luan mot toi uu co giu chat luong.
Reference kernel co the sai khac vai LSB so voi ESP-NN; khong thay reference
mode trong firmware van hanh neu chua do sai so vector va toc do.

## Exact 1x1 fast path

Ba Conv 1x1 co 16->8 va 48->8 kenh tung tao feature map sai khi dung
ESP-NN, nen firmware dung phep tinh int8 tuong duong reference cho rieng
chung. O patch dau tien, moi op tinh ca hai cach va so sanh toan bo output
tung byte. Log `[VEDC CONV] exact 1x1 validated ... fast path enabled` cho
thay op do duoc tang toc tu patch tiep theo; neu buffer khong du hoac output
khac, op tu giu kernel reference de bao ve vector.

Profiler mac dinh chon patch 0, nen thoi gian ba op tren patch nay van la
reference. De do toc do thuc, xem `[AI PERF] total_invoke` va `avg` tren
ca 117 patch, hoac chon `VEDC_AI_OP_PROFILE_INDEX=1` khi can xem thoi gian
tung op sau khi da xac nhan. So sanh voi log cu: 26.546 giay Invoke va
28.909 giay tu CAPTURE START den FEATURES READY. Van can do lai tren board
ESP32-S3 that; build va host parity test khong do duoc toc do thiet bi.

Host parity test: build `tools/test_exact_pointwise_host.cpp` voi
`tensorflow/lite/kernels/internal/common.cc` cua component va chay. Test
so sanh tung byte voi reference cho 200 bo du lieu cua moi dang lop.
