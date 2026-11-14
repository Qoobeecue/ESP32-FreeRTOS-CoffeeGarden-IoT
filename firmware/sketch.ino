#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <ArduinoJson.h>

// ===================== 1. ĐỊNH NGHĨA CHÂN PHẦN CỨNG =====================
#define PIN_DHT       15
#define PIN_MIST_PWM  18  // LED Cyan: Đại diện Máy phun sương (PWM)
#define PIN_PUMP      19  // LED Green: Đại diện Máy bơm nước
#define PIN_FAN       21  // LED Red: Đại diện Quạt thông gió

#define PWM_FREQ      5000
#define PWM_RES       8
#define PWM_CHANNEL   0

// ===================== 2. CẤU HÌNH WIFI & MQTT BROKER ===================
const char* HOME_WIFI_SSID = "TEN_WIFI_NHA_BAN";
const char* HOME_WIFI_PASS = "MAT_KHAU_WIFI_NHA_BAN";

const char* WOKWI_SSID     = "Wokwi-GUEST";
const char* WOKWI_PASS     = "";

const char* MQTT_BROKER    = "broker.emqx.io";
const int   MQTT_PORT      = 1883;

const char* TOPIC_TELEMETRY = "vuon/cafe_hotieu/telemetry";
const char* TOPIC_TEMP      = "vuon/cafe_hotieu/sensor/temp";
const char* TOPIC_HUM       = "vuon/cafe_hotieu/sensor/hum";
const char* TOPIC_STATUS    = "vuon/cafe_hotieu/status";
const char* TOPIC_LWT       = "vuon/cafe_hotieu/lwt";
const char* TOPIC_CMD_PUMP  = "vuon/cafe_hotieu/cmd/pump";
const char* TOPIC_CMD_FAN   = "vuon/cafe_hotieu/cmd/fan";
const char* TOPIC_CMD_MODE  = "vuon/cafe_hotieu/cmd/mode";

// ===================== 3. CẤU TRÚC DỮ LIỆU & RTOS HANDLES ===============
typedef struct {
  float temp;
  float hum;
  uint32_t ts;
  bool sensorError;
} SensorData_t;

QueueHandle_t dataQueue;
QueueHandle_t dataAlert;
SemaphoreHandle_t serialMutex;

volatile bool isAutoMode     = true;
volatile bool pumpState      = false;
volatile bool fanState       = false;
volatile int  mistPowerPct   = 0;
volatile bool stateChanged   = true;
String mistStatusText        = "ON DINH (40% - 60%)";

enum MistState_t {
  MIST_IDLE,
  MIST_ACTIVE,
  MIST_COOLDOWN
};
volatile MistState_t currentMistState = MIST_IDLE;
unsigned long stateStartTime = 0;
unsigned long activeDurationMs = 0;
const unsigned long COOLDOWN_DURATION_MS = 30000;
bool needHysteresisRecovery = false;

DHTesp dht;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ===================== 4. HÀM TIỆN ÍCH ===================================
void setMistPWM(int percent) {
  mistPowerPct = percent;
  int duty = (percent * 255) / 100;
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(PIN_MIST_PWM, duty);
#else
  ledcWrite(PWM_CHANNEL, duty);
#endif
}

void safePrintln(String msg) {
  if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    Serial.println(msg);
    xSemaphoreGive(serialMutex);
  }
}

// ===================== 5. CALLBACK NHẬN LỆNH TỪ WEB ======================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim();
  String topicStr = String(topic);

  // 1. Đổi chế độ AUTO / MANUAL
  if (topicStr == TOPIC_CMD_MODE) {
    if (msg.equalsIgnoreCase("MANUAL")) {
      isAutoMode = false;
      currentMistState = MIST_IDLE;
      needHysteresisRecovery = false;
      setMistPWM(0);
      mistStatusText = "MANUAL MODE";
      safePrintln("[Che do] -> CHUYEN SANG CHE DO THU CONG (MANUAL)");
    } else if (msg.equalsIgnoreCase("AUTO")) {
      isAutoMode = true;
      currentMistState = MIST_IDLE;
      mistStatusText = "AUTO STANDBY";
      safePrintln("[Che do] -> CHUYEN SANG CHE DO TU DONG (AUTO)");
    }
    stateChanged = true;
  }
  // 2. Điều khiển Máy bơm (Tự động chuyển sang MANUAL để người dùng toàn quyền)
  else if (topicStr == TOPIC_CMD_PUMP) {
    isAutoMode = false;
    currentMistState = MIST_IDLE;
    setMistPWM(0);
    mistStatusText = "MANUAL MODE";

    if (msg.equalsIgnoreCase("ON") || msg == "1") {
      pumpState = true;
      digitalWrite(PIN_PUMP, HIGH);
      safePrintln("[Dieu khien] -> MAY BOM: DANG HOAT DONG (ON) | Bom: DANG CHAY (ON) | Quat: " + String(fanState ? "DANG CHAY (ON)" : "TAT (OFF)"));
    } else if (msg.equalsIgnoreCase("OFF") || msg == "0") {
      pumpState = false;
      digitalWrite(PIN_PUMP, LOW);
      safePrintln("[Dieu khien] -> MAY BOM: DA TAT (OFF) | Bom: TAT (OFF) | Quat: " + String(fanState ? "DANG CHAY (ON)" : "TAT (OFF)"));
    }
    stateChanged = true;
  }
  // 3. Điều khiển Quạt thông gió (Tự động chuyển sang MANUAL để người dùng toàn quyền)
  else if (topicStr == TOPIC_CMD_FAN) {
    isAutoMode = false;
    currentMistState = MIST_IDLE;
    setMistPWM(0);
    mistStatusText = "MANUAL MODE";

    if (msg.equalsIgnoreCase("ON") || msg == "1") {
      fanState = true;
      digitalWrite(PIN_FAN, HIGH);
      safePrintln("[Dieu khien] -> QUAT THONG GIO: DANG HOAT DONG (ON) | Bom: " + String(pumpState ? "DANG CHAY (ON)" : "TAT (OFF)") + " | Quat: DANG CHAY (ON)");
    } else if (msg.equalsIgnoreCase("OFF") || msg == "0") {
      fanState = false;
      digitalWrite(PIN_FAN, LOW);
      safePrintln("[Dieu khien] -> QUAT THONG GIO: DA TAT (OFF) | Bom: " + String(pumpState ? "DANG CHAY (ON)" : "TAT (OFF)") + " | Quat: TAT (OFF)");
    }
    stateChanged = true;
  }
}

void publishDeviceStatus() {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["mode"]       = isAutoMode ? "AUTO" : "MANUAL";
  doc["pump"]       = pumpState ? "ON" : "OFF";
  doc["fan"]        = fanState ? "ON" : "OFF";
  doc["mist"]       = mistPowerPct;
  doc["mistStatus"] = mistStatusText;

  char buffer[256];
  serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_STATUS, buffer, true);
}

// ===================== 6. TASK 1: ĐỌC CẢM BIẾN (CORE 1) ==================
void TaskReadSensor(void* pvParameters) {
  float tempBuf[3] = {0}, humBuf[3] = {0};
  int bufIdx = 0, validCount = 0, errorStreak = 0;

  for (;;) {
    TempAndHumidity raw = dht.getTempAndHumidity();

    if (isnan(raw.temperature) || isnan(raw.humidity)) {
      errorStreak++;
      safePrintln("[Task Doc Data - Core " + String(xPortGetCoreID()) + "] CANH BAO: Loi doc cam bien DHT22! (Lan " + String(errorStreak) + ")");
      
      if (errorStreak >= 5) {
        SensorData_t errData = {0.0, -1.0, millis() / 1000, true};
        xQueueSend(dataQueue, &errData, pdMS_TO_TICKS(50));
        float safeHum = 50.0;
        xQueueOverwrite(dataAlert, &safeHum);
      }
    } else {
      errorStreak = 0;
      int prevIdx = (bufIdx + 2) % 3;

      if (validCount > 0 && (abs(raw.humidity - humBuf[prevIdx]) > 2.0 || abs(raw.temperature - tempBuf[prevIdx]) > 1.0)) {
        for (int i = 0; i < 3; i++) {
          tempBuf[i] = raw.temperature;
          humBuf[i]  = raw.humidity;
        }
        validCount = 3;
      } else {
        tempBuf[bufIdx] = raw.temperature;
        humBuf[bufIdx]  = raw.humidity;
        bufIdx = (bufIdx + 1) % 3;
        if (validCount < 3) validCount++;
      }

      float avgTemp = 0, avgHum = 0;
      for (int i = 0; i < validCount; i++) {
        avgTemp += tempBuf[i];
        avgHum  += humBuf[i];
      }
      avgTemp /= validCount;
      avgHum  /= validCount;

      SensorData_t cleanData;
      cleanData.temp        = round(avgTemp * 10.0) / 10.0;
      cleanData.hum         = round(avgHum * 10.0) / 10.0;
      cleanData.ts          = millis() / 1000;
      cleanData.sensorError = false;

      // Khớp chính xác chuỗi định dạng ở Kịch bản 1 & 6
      String logMsg = "[Task Doc Data - Core " + String(xPortGetCoreID()) + "] "
                    + "Nhiet do: " + String(cleanData.temp, 1) + " C | "
                    + "Do am: " + String(cleanData.hum, 1) + " % | "
                    + "Che do: " + (isAutoMode ? "AUTO" : "MANUAL")
                    + " | Phun suong: " + String(mistPowerPct) + "%"
                    + " | Bom: " + (pumpState ? "DANG CHAY (ON)" : "TAT (OFF)")
                    + " | Quat: " + (fanState ? "DANG CHAY (ON)" : "TAT (OFF)");
      safePrintln(logMsg);

      xQueueSend(dataQueue, &cleanData, pdMS_TO_TICKS(50));
      xQueueOverwrite(dataAlert, &cleanData.hum);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ===================== 7. TASK 2: CẢNH BÁO & ĐIỀU KHIỂN (CORE 1) =========
void TaskAlertControl(void* pvParameters) {
  float currentHum = 52.0;

  for (;;) {
    xQueueReceive(dataAlert, &currentHum, pdMS_TO_TICKS(50));
    unsigned long now = millis();

    if (isAutoMode) {
      // Kịch bản 6a: Quá ẩm (> 60%) -> Tắt phun sương, tự động bật Quạt thông gió
      if (currentHum > 60.0) {
        if (currentMistState != MIST_IDLE || mistPowerPct > 0) {
          setMistPWM(0);
          currentMistState = MIST_IDLE;
          needHysteresisRecovery = false;
        }
        if (!fanState) {
          fanState = true;
          digitalWrite(PIN_FAN, HIGH);
          stateChanged = true;
          safePrintln("[Task Canh Bao - Core " + String(xPortGetCoreID()) + "] Do am > 60%! Tat phun suong & Bat Quat chong nam.");
        }
        mistStatusText = "QUA AM (>60%) - DANG THONG GIO";
      } 
      else {
        if (fanState && currentHum <= 58.0) {
          fanState = false;
          digitalWrite(PIN_FAN, LOW);
          stateChanged = true;
          safePrintln("[Task Canh Bao - Core " + String(xPortGetCoreID()) + "] Do am an toan (<= 58%) -> Tat Quat chong nam.");
        }

        // Kịch bản 5: Ngưỡng nghỉ Hysteresis >= 45.0%
        if (currentHum >= 45.0) {
          needHysteresisRecovery = false;
          if (currentMistState == MIST_IDLE && mistPowerPct == 0) {
            mistStatusText = "ON DINH (40% - 60%)";
          }
        }

        switch (currentMistState) {
          case MIST_IDLE:
            if (currentHum < 40.0 || (needHysteresisRecovery && currentHum < 45.0)) {
              needHysteresisRecovery = true;
              stateStartTime = now;
              currentMistState = MIST_ACTIVE;

              if (currentHum < 30.0) {
                // Kịch bản 4: Mức 3 (< 30%) -> 100% trong 30s
                setMistPWM(100);
                activeDurationMs = 30000;
                mistStatusText = "PHUN 100% (30s) - Am < 30%";
                safePrintln("[Task Canh Bao - Core " + String(xPortGetCoreID()) + "] CANH BAO DO (<30%): Bat phun suong 100% trong 30s!");
              } 
              else if (currentHum < 35.0) {
                // Kịch bản 3: Mức 2 (30% - 35%) -> 50% trong 20s
                setMistPWM(50);
                activeDurationMs = 20000;
                mistStatusText = "PHUN 50% (20s) - Am 30-35%";
                safePrintln("[Task Canh Bao - Core " + String(xPortGetCoreID()) + "] CANH BAO VANG (30-35%): Bat phun suong 50% trong 20s!");
              } 
              else {
                // Kịch bản 2 & 5: Mức 1 (35% - 40% hoặc bù Hysteresis 40% - 45%) -> 25% trong 10s
                setMistPWM(25);
                activeDurationMs = 10000;
                mistStatusText = "PHUN 25% (10s) - Am 35-40%";
                safePrintln("[Task Canh Bao - Core " + String(xPortGetCoreID()) + "] CANH BAO NHE (35-40%): Bat phun suong 25% trong 10s!");
              }
              stateChanged = true;
            }
            break;

          case MIST_ACTIVE:
            if (now - stateStartTime >= activeDurationMs) {
              setMistPWM(0);
              currentMistState = MIST_COOLDOWN;
              stateStartTime = now;
              mistStatusText = "COOLDOWN (Nghi 30s cho khuech tan)";
              stateChanged = true;
              safePrintln("[Task Canh Bao - Core " + String(xPortGetCoreID()) + "] Hoan tat chu ky phun -> Chuyen sang Cooldown 30s.");
            }
            break;

          case MIST_COOLDOWN:
            if (now - stateStartTime >= COOLDOWN_DURATION_MS) {
              currentMistState = MIST_IDLE;
              mistStatusText = "DANG DANH GIA LAI DO AM";
              stateChanged = true;
              safePrintln("[Task Canh Bao - Core " + String(xPortGetCoreID()) + "] Het thoi gian Cooldown -> San sang chu ky do moi.");
            }
            break;
        }
      }
    }
  }
}

// ===================== 8. TASK 3: TRUYỀN NHẬN WIFI & MQTT (CORE 0) =======
void TaskMqttComm(void* pvParameters) {
  unsigned long lastWifiCheck = 0;
  unsigned long lastMqttRetry = 0;
  bool useWokwiWifi = false;
  SensorData_t rxData;

  WiFi.mode(WIFI_STA);

  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == String(WOKWI_SSID)) {
      useWokwiWifi = true;
      break;
    }
  }

  if (useWokwiWifi) {
    safePrintln("[Task Truyen Data - Core 0] Ket noi qua mang may tinh (Wokwi-GUEST)...");
    WiFi.begin(WOKWI_SSID, WOKWI_PASS);
  } else {
    safePrintln("[Task Truyen Data - Core 0] Ket noi WiFi: " + String(HOME_WIFI_SSID));
    WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASS);
  }

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  for (;;) {
    unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastWifiCheck >= 5000) {
        lastWifiCheck = now;
        safePrintln("[Task Truyen Data - Core 0] Dang thu ket noi lai WiFi...");
        WiFi.reconnect();
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!mqttClient.connected()) {
      if (now - lastMqttRetry >= 5000) {
        lastMqttRetry = now;
        String clientId = "ESP32_CoffeeGarden_" + String(random(1000, 9999));
        safePrintln("[Task Truyen Data - Core 0] Dang ket noi MQTT Broker...");

        if (mqttClient.connect(clientId.c_str(), TOPIC_LWT, 1, true, "offline")) {
          safePrintln("[Task Truyen Data - Core 0] Da ket noi MQTT thanh cong!");
          mqttClient.publish(TOPIC_LWT, "online", true);

          mqttClient.subscribe(TOPIC_CMD_PUMP, 1);
          mqttClient.subscribe(TOPIC_CMD_FAN, 1);
          mqttClient.subscribe(TOPIC_CMD_MODE, 1);

          publishDeviceStatus();
        } else {
          safePrintln("[Task Truyen Data - Core 0] Loi MQTT rc=" + String(mqttClient.state()));
        }
      }
    } else {
      mqttClient.loop();

      if (stateChanged) {
        stateChanged = false;
        publishDeviceStatus();
      }

      if (xQueueReceive(dataQueue, &rxData, 0) == pdPASS) {
        mqttClient.publish(TOPIC_TEMP, String(rxData.temp, 1).c_str());
        mqttClient.publish(TOPIC_HUM, String(rxData.hum, 1).c_str());

        JsonDocument doc;
        doc["t"]          = rxData.temp;
        doc["h"]          = rxData.hum;
        doc["mist"]       = mistPowerPct;
        doc["mistStatus"] = mistStatusText;
        doc["pump"]       = pumpState ? "ON" : "OFF";
        doc["fan"]        = fanState ? "ON" : "OFF";
        doc["mode"]       = isAutoMode ? "AUTO" : "MANUAL";
        doc["error"]      = rxData.sensorError;
        doc["ts"]         = rxData.ts;

        char jsonBuffer[256];
        serializeJson(doc, jsonBuffer);
        mqttClient.publish(TOPIC_TELEMETRY, jsonBuffer);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ===================== 9. HÀM KHỞI TẠO HỆ THỐNG (SETUP) ==================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=======================================================");
  Serial.println("HE THONG GIAM SAT & DIEU KHIEN VUON CA PHE / HO TIEU");
  Serial.println("=======================================================");

  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_FAN, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  digitalWrite(PIN_FAN, LOW);

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcAttach(PIN_MIST_PWM, PWM_FREQ, PWM_RES);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_MIST_PWM, PWM_CHANNEL);
#endif
  setMistPWM(0);

  dht.setup(PIN_DHT, DHTesp::DHT22);

  dataQueue   = xQueueCreate(10, sizeof(SensorData_t));
  dataAlert   = xQueueCreate(1, sizeof(float));
  serialMutex = xSemaphoreCreateMutex();

  if (dataQueue == NULL || dataAlert == NULL || serialMutex == NULL) {
    while (1);
  }

  BaseType_t appCore = (portNUM_PROCESSORS > 1) ? 1 : 0;

  xTaskCreatePinnedToCore(TaskMqttComm, "Task_TruyenData", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskReadSensor, "Task_DocData", 4096, NULL, 2, NULL, appCore);
  xTaskCreatePinnedToCore(TaskAlertControl, "Task_CanhBao", 4096, NULL, 3, NULL, appCore);
}

void loop() {
  vTaskDelete(NULL);
}