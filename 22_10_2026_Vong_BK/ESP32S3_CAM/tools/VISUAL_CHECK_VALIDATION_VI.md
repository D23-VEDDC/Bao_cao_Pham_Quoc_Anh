# Kiểm chứng ngoại quan bằng Memory Bank AI

Với frame 640×480, firmware tìm bốn góc PCB xanh rồi chuẩn hóa thành RGB 640×448.
Lưới 13 cột × 9 hàng = 117 patch RGB 64×64, stride 48 (chồng lấp 16 pixel).
Giữ trọn mép PCB trong ảnh gốc để bước tìm góc hoạt động đúng.
Log `pool_fnv1a` của capture và lần xử lý lại đúng crop RGB phải trùng nhau;
`/api/status` xuất `check.scores`, `check.d1`, `check.d3`, threshold và tọa độ crop.
Ngưỡng tạo từ 5 ảnh LEARN chỉ là ngưỡng khởi đầu; chụp thêm ảnh tốt độc lập
để chọn mức báo sai chấp nhận được. Giữ BMP `/api/normalized-image`
và JSON cùng capture_id.

Firmware dùng model TFLite tạo vector 32 chiều cho mỗi patch 64×64 (stride 48),
LEARN chọn vector đại diện bằng greedy k-center, CHECK so từng vector với
Memory Bank toàn cục. Ảnh test không cần trùng vị trí với ảnh LEARN.

Điểm CHECK dùng láng giềng gần nhất cùng một phần khoảng cách tới láng giềng
thứ ba để tránh một vector trùng ngẫu nhiên che lỗi. Ngưỡng LEARN được hiệu
chỉnh bằng cách lần lượt bỏ một ảnh sạch ra, dựng Memory Bank tạm từ bốn ảnh
còn lại bằng đúng greedy k-center của CHECK, rồi lấy điểm patch lớn nhất của
ảnh được bỏ ra. Sau năm lượt, firmware dựng Memory Bank chính thức từ cả năm
ảnh. Các Memory Bank tạm không được giữ lại để CHECK.
Ngưỡng cuối lấy giá trị lớn hơn giữa điểm cao nhất của năm lượt bỏ một ảnh và
điểm cao nhất khi chấm lại năm ảnh với Memory Bank chính thức, rồi nhân hệ số
dự phòng 1,20. Cách này giữ các ảnh đã học ở phía OK dù coreset cuối chọn
vector khác các coreset tạm.

1. Nạp firmware, reset LEARN và chụp 5 ảnh PCB sạch với mức xê dịch nhẹ thường
   gặp khi vận hành. Giữ ánh sáng và độ cao camera như lúc CHECK. Theo dõi log
   `leave-one-out coreset max`. Nếu TRAIN báo một ảnh là outlier, reset LEARN và chụp
   lại 5 mẫu sạch; firmware không đưa bộ mẫu khác thường vào Memory Bank.
2. CHECK ít nhất 10 ảnh PCB sạch ở các vị trí khác nhau. Lưu `/api/status` và
   BMP `/api/last-image` sau mỗi lượt.
3. CHECK vật lạ ở nhiều vị trí và kích thước. Đối chiếu `check.scores`,
   `check.threshold`, số patch lỗi và khung đỏ với đúng BMP đã chuẩn hóa.

Nếu model cho vector gần như không đổi khi có vật lạ, cần đánh giá hoặc huấn
luyện lại chính feature extractor; chỉnh ngưỡng không thể tạo thông tin mà
vector AI không biểu diễn. Chưa có ảnh PCB mẫu trong kho mã, nên build thành
công không chứng minh độ chính xác phát hiện trên thiết bị.
