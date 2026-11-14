# 🌱 Hệ Thống Giám Sát & Điều Khiển Vi Khí Hậu Vườn Cà Phê / Hồ Tiêu (ESP32 FreeRTOS)

## 1. Giới thiệu đề tài
Hệ thống sử dụng vi điều khiển **ESP32 (Dual-Core FreeRTOS)** kết hợp cảm biến **DHT22**, giao thức **MQTT** và **Web Dashboard** thời gian thực để giám sát nhiệt độ, độ ẩm và tự động điều tiết vi khí hậu cho vườn cây công nghiệp (Cà phê, Hồ tiêu).

## 2. Kiến trúc hệ thống
- **Core 0 (`TaskMqttComm`):** Quản lý kết nối WiFi tự động, duy trì phiên MQTT Broker, gửi dữ liệu Telemetry (JSON) và nhận lệnh điều khiển hai chiều.
- **Core 1 (`TaskReadSensor` & `TaskAlertControl`):** Đọc DHT22 chu kỳ 2s với bộ lọc trung bình trượt thích ứng; vận hành máy trạng thái không chặn điều khiển phun sương PWM 3 mức (`25% / 50% / 100%`), nghỉ khuếch tán `Cooldown 30s`, vùng trễ `Hysteresis (40% - 45%)` và quạt thông gió chống nấm (`> 60%`).

## 3. Link Demo Giao diện Web Dashboard
👉 Truy cập trực tiếp tại: **[https://qoobeecue.github.io/ESP32-FreeRTOS-CoffeeGarden-IoT/](https://qoobeecue.github.io/ESP32-FreeRTOS-CoffeeGarden-IoT/)**
⚡ **Mô phỏng phần cứng Wokwi: **https://wokwi.com/projects/480603111342616577**
