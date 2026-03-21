# Đèn ngủ Hành tinh thông minh

Đây là dự án đèn ngủ mô phỏng hành tinh thông minh, sử dụng vi điều khiển ESP8266 hoặc ESP32 để điều khiển dải LED RGB NeoPixel kết hợp với cảm biến khoảng cách và cảm biến ánh sáng. Sản phẩm hướng tới việc tạo ra một mô hình **Trái Đất, Sao Thổ, Mặt Trăng hoặc các hành tinh khác** từ **vật liệu tái chế**, vừa có giá trị trang trí, vừa có tính ứng dụng thực tế trong không gian học tập, phòng ngủ hoặc khu trưng bày STEM.

Điểm nổi bật của dự án là mô hình không chỉ phát sáng thông thường mà còn có khả năng phản hồi theo môi trường xung quanh:

- Khi trời tối, hành tinh tự phát sáng.
- Khi người dùng lại gần, đèn sáng mạnh hơn.
- Khi môi trường đủ sáng, đèn có thể giảm hoặc ngừng phát sáng để tiết kiệm năng lượng.
- Người dùng có thể đổi màu đèn và cấu hình thiết bị qua giao diện web nội bộ.

## Ý tưởng dự án

“Đèn ngủ Hành tinh thông minh” là một mô hình trang trí kết hợp công nghệ cảm biến. Phần vỏ ngoài có thể được làm từ các vật liệu tái chế như giấy, bìa carton, nhựa mỏng, xốp, bóng nhựa cũ hoặc các vật liệu thủ công khác. Phần điện tử bên trong gồm LED RGB, bo mạch điều khiển WiFi, cảm biến siêu âm và cảm biến ánh sáng.

Mục tiêu của dự án là tạo ra một sản phẩm:

- Đẹp về thẩm mỹ.
- Có tính tương tác với người dùng.
- Tận dụng vật liệu tái chế để tăng tính sáng tạo và thân thiện với môi trường.
- Có thể mở rộng thành sản phẩm STEM, đồ án học sinh, sản phẩm trưng bày hoặc quà tặng công nghệ.

## Cơ chế hoạt động

Hệ thống hoạt động theo hai điều kiện chính:

### 1. Khi trời tối, hành tinh tự phát sáng

Cảm biến ánh sáng sẽ theo dõi trạng thái môi trường. Khi ánh sáng xung quanh giảm xuống mức thấp, hệ thống cho phép LED phát sáng để biến mô hình hành tinh thành đèn ngủ thông minh. Khi môi trường bên ngoài sáng trở lại, đèn có thể tắt hoặc bị chặn xuất sáng tùy theo cấu hình.

### 2. Khi người dùng lại gần, đèn sáng mạnh hơn

Cảm biến siêu âm đo khoảng cách giữa người dùng và mô hình. Khi phát hiện có vật thể hoặc bàn tay tiến lại gần, hệ thống tăng độ sáng LED. Khi người dùng đứng xa hơn, độ sáng giảm dần về mức thấp hơn để tạo hiệu ứng mềm mại, tự nhiên và tiết kiệm điện năng.

## Chức năng hiện có trong chương trình

- Điều khiển màu LED RGB.
- Chọn màu có sẵn hoặc nhập mã màu HEX tùy chỉnh.
- Điều chỉnh độ sáng theo khoảng cách đo từ cảm biến siêu âm.
- Kích hoạt hoặc chặn phát sáng theo trạng thái sáng/tối của môi trường.
- Lưu cấu hình WiFi vào EEPROM để thiết bị tự nhớ sau khi khởi động lại.
- Phát WiFi Access Point để điều khiển bằng điện thoại hoặc máy tính.
- Có trang cấu hình WiFi riêng để kết nối thêm vào mạng WiFi gia đình.
- Hỗ trợ nút nhấn `GPIO0 / FLASH` để thao tác nhanh.

## Ứng dụng thực tế

Dự án này phù hợp cho nhiều mục đích:

- Làm đèn ngủ trang trí cho phòng ngủ.
- Làm mô hình STEM cho học sinh, sinh viên.
- Trưng bày sản phẩm sáng tạo từ vật liệu tái chế.
- Trình diễn cảm biến và điều khiển thông minh bằng vi điều khiển.
- Phát triển thành sản phẩm quà tặng công nghệ hoặc đồ handmade thông minh.

## Phần cứng sử dụng

- Bo mạch ESP8266 hoặc ESP32 có WiFi.
- Dải LED RGB NeoPixel / WS2812.
- Cảm biến siêu âm đo khoảng cách.
- Cảm biến ánh sáng số.
- Nút nhấn `FLASH / GPIO0`.
- Nguồn cấp phù hợp cho LED và vi điều khiển.
- Vỏ mô hình hành tinh làm từ vật liệu tái chế.

## Sơ đồ chân mặc định trong mã nguồn

- `D5`: Tín hiệu điều khiển LED NeoPixel
- `D6`: Chân `TRIG` của cảm biến siêu âm
- `D7`: Chân `ECHO` của cảm biến siêu âm
- `D1`: Tín hiệu từ cảm biến ánh sáng
- `GPIO0`: Nút nhấn chuyển màu nhanh hoặc khôi phục WiFi mặc định

Nếu phần cứng thực tế khác với cấu hình trên, có thể chỉnh lại các hằng số ở đầu file `DuoiLedRGB.ino`.

## Giao diện điều khiển

Thiết bị tự tạo một mạng WiFi riêng để người dùng truy cập giao diện điều khiển. Từ giao diện web, người dùng có thể:

- Xem trạng thái độ sáng, màu sắc và khoảng cách đo được.
- Đổi màu đèn.
- Chọn màu tùy chỉnh.
- Điều chỉnh chế độ độ sáng cố định hoặc theo khoảng cách.
- Cấu hình WiFi của thiết bị.

## Thông tin WiFi mặc định

- Tên WiFi AP: `HanhTinhThongMinh`
- Mật khẩu AP: `hanhtinh123`

Sau khi nạp chương trình, chỉ cần kết nối điện thoại hoặc máy tính vào mạng WiFi này để truy cập giao diện điều khiển của đèn.

## Thư viện cần cài đặt

- `Adafruit NeoPixel`
- `EEPROM`
- Gói board `ESP8266` hoặc `ESP32` trong Arduino IDE

## Cách nạp chương trình

1. Mở file `DuoiLedRGB.ino` bằng Arduino IDE.
2. Chọn đúng loại board ESP8266 hoặc ESP32.
3. Chọn đúng cổng COM đang kết nối.
4. Cài các thư viện cần thiết nếu máy chưa có.
5. Kiểm tra lại chân kết nối phần cứng.
6. Nạp chương trình lên bo mạch.
7. Kết nối vào WiFi của thiết bị để sử dụng.

## Cấu trúc dự án

- `DuoiLedRGB.ino`: Chương trình chính điều khiển toàn bộ hệ thống.
- `README.md`: Tài liệu mô tả dự án.
- `LICENSE`: Thông tin bản quyền của dự án.
- `.gitignore`: Danh sách file tạm và file build không đưa lên GitHub.

## Định hướng mở rộng

Trong tương lai, dự án có thể mở rộng thêm:

- Hiệu ứng ánh sáng theo từng loại hành tinh.
- Tự động đổi màu theo thời gian.
- Tích hợp cảm biến chạm hoặc cảm biến chuyển động.
- Điều khiển qua ứng dụng điện thoại.
- Đồng bộ thời gian thực để mô phỏng ngày và đêm.
- Thêm loa hoặc âm thanh không gian để tăng trải nghiệm.

## Bản quyền

Copyright (c) 2026 TranDangKhoaTechnology.  
All rights reserved.

Xem thêm trong file `LICENSE`.
