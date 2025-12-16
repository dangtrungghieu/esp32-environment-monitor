/*
 * ========================================
 * AC CONTROL - ESP32 FIRMWARE v1.0
 * ========================================
 * Tính năng:
 * - Setup qua WiFi AP (web interface)
 * - Lưu Device ID vào EEPROM
 * - Kết nối Firebase Realtime Database
 * - Điều khiển AC qua IR (hỗ trợ thư viện + học lệnh)
 * - Đọc DHT22 (nhiệt độ, độ ẩm)
 * - Đọc HLK-LD2410C (phát hiện người)
 * - Đọc PZEM-004T (công suất)
 * - Reset bằng nút nhấn (GPIO 15, giữ 5s)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <FirebaseESP32.h>
#include <DHT.h>
#include <PZEM004Tv30.h>
#include <time.h>

// IR Libraries
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <ir_Daikin.h>
#include <ir_Panasonic.h>
#include <ir_Mitsubishi.h>
#include <ir_LG.h>
#include <ir_Samsung.h>
#include <ir_Toshiba.h>

// ═══════════════════════════════════════
// CẤU HÌNH PHẦN CỨNG (Dựa trên code test)
// ═══════════════════════════════════════
#define IR_SEND_PIN       27    // IR LED (qua transistor)
#define IR_RECV_PIN       26    // IR Receiver
#define DHT_PIN           33     // DHT22
#define PZEM_RX_PIN       16    // PZEM TX → ESP RX
#define PZEM_TX_PIN       17    // PZEM RX → ESP TX
#define HLK_OUT_PIN       25    // HLK-LD2410C OUT
#define RESET_BUTTON_PIN  15    // Nút reset (PULLDOWN)
#define LED_PIN           2     // LED built-in

#define DHT_TYPE          DHT22
#define CAPTURE_BUFFER_SIZE 512  // Giảm từ 1200 → 512 để tiết kiệm RAM
#define IR_TIMEOUT_MS     15

// ═══════════════════════════════════════
// CẤU HÌNH EEPROM
// ═══════════════════════════════════════
#define EEPROM_SIZE              512
#define EEPROM_DEVICE_ID_ADDR    0      // 0-49: Device ID
#define EEPROM_CLAIM_CODE_ADDR   50     // 50-69: Claim Code
#define EEPROM_WIFI_SSID_ADDR    70     // 70-101: WiFi SSID
#define EEPROM_WIFI_PASS_ADDR    102    // 102-165: WiFi Password
#define EEPROM_INITIALIZED_ADDR  166    // 166: Flag

// ═══════════════════════════════════════
// CẤU HÌNH FIREBASE
// ═══════════════════════════════════════
#define FIREBASE_HOST "ac-control-db-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "TSVCjAw2PJWDSofxbJwNwU3fXr5fXXs4T18VHDwa"  
#define FIRMWARE_VERSION "1.0.0"
// ═══════════════════════════════════════
// BIẾN TOÀN CỤC
// ═══════════════════════════════════════

// Device Info
String deviceId = "";
String claimCode = "";
String wifiSSID = "";
String wifiPassword = "";
bool isSetupMode = false;

// Firebase
FirebaseData firebaseData;
FirebaseData streamData;
FirebaseConfig firebaseConfig;
FirebaseAuth firebaseAuth;

// Web Server
WebServer server(80);

// Sensors
DHT dht(DHT_PIN, DHT_TYPE);
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);

// IR
IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN, CAPTURE_BUFFER_SIZE, IR_TIMEOUT_MS, true);
decode_results irResults;

// IR AC Objects
String currentProtocol = "";
IRDaikinESP* daikinAC = nullptr;
IRPanasonicAc* panasonicAC = nullptr;
IRMitsubishiAC* mitsubishiAC = nullptr;
IRLgAc* lgAC = nullptr;
IRSamsungAc* samsungAC = nullptr;
IRToshibaAC* toshibaAC = nullptr;

// IR Learned Commands - 4 lệnh riêng biệt
uint16_t learnedPowerOn[CAPTURE_BUFFER_SIZE];
uint16_t learnedPowerOnLen = 0;
uint16_t learnedPowerOff[CAPTURE_BUFFER_SIZE];
uint16_t learnedPowerOffLen = 0;
uint16_t learnedTempUp[CAPTURE_BUFFER_SIZE];
uint16_t learnedTempUpLen = 0;
uint16_t learnedTempDown[CAPTURE_BUFFER_SIZE];
uint16_t learnedTempDownLen = 0;
uint16_t irFrequency = 38;

// AC State
bool acPowerOn = false;
int currentTemp = 24;

// Sensor Data
float roomTemp = 0;
float roomHumidity = 0;
bool personDetected = false;
float powerCurrent = 0;
float powerWatt = 0;

// DHT22 Error Recovery
int dhtErrorCount = 0;
const int DHT_MAX_ERRORS = 2; // Sau 2 lần lỗi liên tiếp → reset DHT

// PZEM Error Recovery
int pzemErrorCount = 0;
const int PZEM_MAX_ERRORS = 3; // Sau 3 lần lỗi liên tiếp → reset PZEM
int pzemStuckCount = 0; // Đếm số lần giá trị bị đứng
const int PZEM_STUCK_THRESHOLD = 5; // Nếu giá trị giống hệt nhau 5 lần → reset
float lastValidVoltage = 0;
float lastValidCurrent = 0;
float lastValidPower = 0;

// Timing
unsigned long lastSensorRead = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastHeartbeat = 0;

// Reset Button (dựa trên code test)
bool lastButtonState = LOW;
unsigned long lastDebounce = 0;
unsigned long resetButtonPressStart = 0;
const unsigned long debounceMs = 50;
const unsigned long resetHoldTime = 5000; // 5 giây

// ═══════════════════════════════════════
// FORWARD DECLARATIONS
// ═══════════════════════════════════════
void printBanner();
bool isDeviceConfigured();
void loadConfiguration();
void saveConfiguration();
void factoryReset();
void checkResetButton();
void runSetupMode();
void setupWebServer();
String getSetupHTML();
String getSuccessHTML();
void connectWiFi();
void syncTime();
void connectFirebase();
void resetDHT22();
void resetPZEM();
void readSensors();
void updateStatus();
void sendHeartbeat();
void initializeIRProtocol(String protocol);
void sendIRLibraryCommand(String action);
void sendIRLearnedCommand(String action);
void checkCommands();
void loadLearnedCommands();
void parseRawData(String data, uint16_t* buffer, uint16_t& len);
void checkIRLearning();
void claimDevice();

// ═══════════════════════════════════════
// SETUP
// ═══════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(1000);
  // Xóa buffer Serial
  while (Serial.available()) {
    Serial.read();
  }
  
  printBanner();
  
  
  // Khởi tạo phần cứng
  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(HLK_OUT_PIN, INPUT);
  
  digitalWrite(LED_PIN, LOW);
  
  // Khởi tạo EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Khởi tạo sensors
  dht.begin();
  irsend.begin();
  irrecv.enableIRIn();
  
  Serial.println("✓ Cam bien DHT22 khoi dong");
  Serial.println("✓ Cam bien PZEM-004T khoi dong");
  Serial.println("✓ Cam bien HLK-LD2410C khoi dong");
  Serial.println("✓ IR Sender khoi dong");
  Serial.println("✓ IR Receiver khoi dong");
  Serial.println();
  
  // Kiểm tra cấu hình
  if (isDeviceConfigured()) {
    loadConfiguration();
    
    Serial.println("═════════════════════════════════════");
    Serial.println("  THIET BI DA DUOC CAU HINH");
    Serial.println("═════════════════════════════════════");
    Serial.print("Device ID:   ");
    Serial.println(deviceId);
    Serial.print("Claim Code:  ");
    Serial.println(claimCode);
    Serial.print("WiFi SSID:   ");
    Serial.println(wifiSSID);
    Serial.println("═════════════════════════════════════");
    Serial.println();
    
    // Kết nối WiFi
    connectWiFi();

    // Đồng bộ thời gian (QUAN TRỌNG cho SSL)
    syncTime();

    // Kết nối Firebase
    connectFirebase();

    // Kiểm tra và claim device
    claimDevice();
    
    // Chạy chế độ bình thường
    Serial.println(">>> CHE DO BINH THUONG <<<");
    Serial.println();
    
  } else {
    Serial.println("═════════════════════════════════════");
    Serial.println("  THIET BI CHUA DUOC CAU HINH");
    Serial.println("  VAO CHE DO SETUP...");
    Serial.println("═════════════════════════════════════");
    Serial.println();
    
    // Chạy chế độ setup
    runSetupMode();
  }
}

// ═══════════════════════════════════════
// LOOP
// ═══════════════════════════════════════
void loop() {
  // Xóa ký tự rác từ Serial
  while (Serial.available()) {
    Serial.read();
  }

  if (isSetupMode) {
    // Chế độ setup: xử lý web server
    server.handleClient();
    checkResetButton();

    // LED nháy chậm trong setup mode (1 giây on, 1 giây off)
    if ((millis() / 1000) % 2 == 0) {
      digitalWrite(LED_PIN, HIGH);
    } else {
      digitalWrite(LED_PIN, LOW);
    }

  } else {
    // Chế độ bình thường
    checkResetButton();
    readSensors();
    updateStatus();
    sendHeartbeat();
    checkCommands();
    checkIRLearning();
  }

  delay(10);
}

// ═══════════════════════════════════════
// EEPROM FUNCTIONS
// ═══════════════════════════════════════
bool isDeviceConfigured() {
  return EEPROM.read(EEPROM_INITIALIZED_ADDR) == 0xAA;
}

void loadConfiguration() {
  // Đọc Device ID
  deviceId = "";
  for (int i = 0; i < 50; i++) {
    char c = EEPROM.read(EEPROM_DEVICE_ID_ADDR + i);
    if (c == 0) break;
    deviceId += c;
  }
  
  // Đọc Claim Code
  claimCode = "";
  for (int i = 0; i < 20; i++) {
    char c = EEPROM.read(EEPROM_CLAIM_CODE_ADDR + i);
    if (c == 0) break;
    claimCode += c;
  }
  
  // Đọc WiFi SSID
  wifiSSID = "";
  for (int i = 0; i < 32; i++) {
    char c = EEPROM.read(EEPROM_WIFI_SSID_ADDR + i);
    if (c == 0) break;
    wifiSSID += c;
  }
  
  // Đọc WiFi Password
  wifiPassword = "";
  for (int i = 0; i < 64; i++) {
    char c = EEPROM.read(EEPROM_WIFI_PASS_ADDR + i);
    if (c == 0) break;
    wifiPassword += c;
  }
}

void saveConfiguration() {
  // Lưu Device ID
  for (int i = 0; i < 50; i++) {
    if (i < deviceId.length()) {
      EEPROM.write(EEPROM_DEVICE_ID_ADDR + i, deviceId[i]);
    } else {
      EEPROM.write(EEPROM_DEVICE_ID_ADDR + i, 0);
    }
  }
  
  // Lưu Claim Code
  for (int i = 0; i < 20; i++) {
    if (i < claimCode.length()) {
      EEPROM.write(EEPROM_CLAIM_CODE_ADDR + i, claimCode[i]);
    } else {
      EEPROM.write(EEPROM_CLAIM_CODE_ADDR + i, 0);
    }
  }
  
  // Lưu WiFi SSID
  for (int i = 0; i < 32; i++) {
    if (i < wifiSSID.length()) {
      EEPROM.write(EEPROM_WIFI_SSID_ADDR + i, wifiSSID[i]);
    } else {
      EEPROM.write(EEPROM_WIFI_SSID_ADDR + i, 0);
    }
  }
  
  // Lưu WiFi Password
  for (int i = 0; i < 64; i++) {
    if (i < wifiPassword.length()) {
      EEPROM.write(EEPROM_WIFI_PASS_ADDR + i, wifiPassword[i]);
    } else {
      EEPROM.write(EEPROM_WIFI_PASS_ADDR + i, 0);
    }
  }
  
  // Đánh dấu đã khởi tạo
  EEPROM.write(EEPROM_INITIALIZED_ADDR, 0xAA);
  
  // Commit
  EEPROM.commit();
  
  Serial.println("✓ Da luu cau hinh vao EEPROM");
}

void factoryReset() {
  Serial.println();
  Serial.println("═════════════════════════════════════");
  Serial.println("  FACTORY RESET");
  Serial.println("═════════════════════════════════════");
  Serial.println("Dang xoa toan bo cau hinh...");

  // LED nháy nhanh báo hiệu đang reset
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
  
  // ===== BƯỚC 1: CẬP NHẬT FIREBASE TRƯỚC KHI RESET =====
  if (deviceId != "" && Firebase.ready()) {
    Serial.println();
    Serial.println(">>> Dang reset thiet bi tren Firebase...");
    
    // Lấy owner cũ
    String oldOwnerId = "";
    if (Firebase.getString(firebaseData, "devices/" + deviceId + "/info/ownerId")) {
      oldOwnerId = firebaseData.stringData();
      Serial.print("Owner cu: ");
      Serial.println(oldOwnerId);
    }
    
    // Reset device info - GỬI 1 LẦN BẰNG JSON
    Serial.println("Dang cap nhat device info...");

    FirebaseJson json;
    json.set("claimed", false);
    json.set("ownerId", "");
    json.set("deviceName", "");
    json.set("roomName", "");
    // KHÔNG set macAddress = "" → Để giữ MAC cũ
    // Điều này giúp phân biệt "device đã reset" vs "device mới"
    json.set("firmwareVersion", "");
    json.set("lastModified", (int)millis());

    String basePath = "devices/" + deviceId + "/info";
    if (Firebase.updateNode(firebaseData, basePath, json)) {
      Serial.println("✓ Da reset device info (1 request)");
    } else {
      Serial.println("✗ Loi reset device info:");
      Serial.println(firebaseData.errorReason());
    }
    delay(1000);
    
    // Xóa khỏi user_devices
    if (oldOwnerId != "") {
      Serial.print("Dang xoa khoi user_devices/");
      Serial.print(oldOwnerId);
      Serial.print("/");
      Serial.println(deviceId);

      Firebase.deleteNode(firebaseData, "user_devices/" + oldOwnerId + "/" + deviceId);
      Serial.println("✓ Da xoa khoi user_devices");
      delay(500);
    }

    // Xóa IR config
    Serial.println("Dang xoa IR config...");
    Firebase.deleteNode(firebaseData, "devices/" + deviceId + "/ir");
    Serial.println("✓ Da xoa IR config");
    delay(500);

    // Xóa status
    Serial.println("Dang xoa status...");
    Firebase.deleteNode(firebaseData, "status/" + deviceId);
    Serial.println("✓ Da xoa status");
    delay(500);

    // Xóa commands
    Serial.println("Dang xoa commands...");
    Firebase.deleteNode(firebaseData, "commands/" + deviceId);
    Serial.println("✓ Da xoa commands");
    delay(500);

    // Xóa ir_learning
    Serial.println("Dang xoa ir_learning...");
    Firebase.deleteNode(firebaseData, "ir_learning/" + deviceId);
    Serial.println("✓ Da xoa ir_learning");
    delay(500);
    
    Serial.println();
    Serial.println("✓✓✓ HOAN THANH reset tren Firebase!");
    Serial.println();
    
    delay(2000);
  } else {
    Serial.println("⚠️ Khong ket noi Firebase hoac chua co deviceId");
    Serial.println("Chi xoa local EEPROM...");
  }
  
  // ===== BƯỚC 2: XÓA EEPROM LOCAL =====
  Serial.println("Dang xoa EEPROM local...");
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0x00);
  }
  EEPROM.commit();
  
  Serial.println("✓ Da xoa EEPROM");
  Serial.println("Thiet bi se khoi dong lai sau 3 giay...");
  Serial.println("═════════════════════════════════════");
  
  delay(3000);
  ESP.restart();
}

// ═══════════════════════════════════════
// NÚT RESET (Dựa trên code test button)
// ═══════════════════════════════════════
void checkResetButton() {
  bool reading = digitalRead(RESET_BUTTON_PIN);

  // Debounce
  if (reading != lastButtonState) {
    lastDebounce = millis();
    lastButtonState = reading;
  }

  if (millis() - lastDebounce >= debounceMs) {
    if (reading == HIGH) {
      // Nút đang được nhấn
      if (resetButtonPressStart == 0) {
        resetButtonPressStart = millis();
        Serial.println("Nut reset duoc nhan...");
      }

      // LED nháy nhanh khi đang giữ nút
      unsigned long holdTime = millis() - resetButtonPressStart;
      if (holdTime < resetHoldTime) {
        // Nháy LED mỗi 200ms
        if ((millis() / 200) % 2 == 0) {
          digitalWrite(LED_PIN, HIGH);
        } else {
          digitalWrite(LED_PIN, LOW);
        }
      }

      // Kiểm tra đã giữ đủ 5s chưa
      if (holdTime >= resetHoldTime) {
        Serial.println("Nut reset giu du 5 giay!");
        factoryReset();
      }
    } else {
      // Nút được thả ra
      if (resetButtonPressStart > 0) {
        unsigned long holdTime = millis() - resetButtonPressStart;
        if (holdTime < resetHoldTime) {
          Serial.print("Nut reset tha ra sau ");
          Serial.print(holdTime / 1000.0, 1);
          Serial.println("s (chua du 5s)");

          // Tắt LED khi thả nút chưa đủ thời gian
          digitalWrite(LED_PIN, LOW);
        }
        resetButtonPressStart = 0;
      }
    }
  }
}

// ═══════════════════════════════════════
// SETUP MODE - WEB SERVER
// ═══════════════════════════════════════
void runSetupMode() {
  isSetupMode = true;
  
  // Tạo SSID duy nhất
  String macAddress = WiFi.macAddress();
  macAddress.replace(":", "");
  String apSSID = "AC-Setup-" + macAddress.substring(6);
  
  Serial.println("═════════════════════════════════════");
  Serial.println("  TAO WIFI ACCESS POINT");
  Serial.println("═════════════════════════════════════");
  Serial.print("SSID:     ");
  Serial.println(apSSID);
  Serial.println("Password: 12345678");
  Serial.println("IP:       192.168.4.1");
  Serial.println("═════════════════════════════════════");
  Serial.println();
  Serial.println("Huong dan:");
  Serial.println("1. Ket noi WiFi: " + apSSID);
  Serial.println("2. Mo trinh duyet: http://192.168.4.1");
  Serial.println("3. Nhap thong tin thiet bi");
  Serial.println();
  
  // Tạo AP
  WiFi.softAP(apSSID.c_str(), "12345678");
  
  // Setup web server
  setupWebServer();
  server.begin();
  
  Serial.println("✓ Web server da san sang!");
  Serial.println();
}

void setupWebServer() {
  // Trang chủ - Scan WiFi
  server.on("/", HTTP_GET, []() {
    String html = getSetupHTML();
    server.send(200, "text/html", html);
  });
  
  // API: Scan WiFi - FIXED VERSION
  server.on("/scan", HTTP_GET, []() {
    Serial.println("Dang scan WiFi...");
    
    // Tạm thời chuyển sang STA mode để scan
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    
    int n = WiFi.scanNetworks();
    
    String json = "[";
    
    if (n > 0) {
      for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        int encryption = WiFi.encryptionType(i);
        
        // Escape JSON string
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        
        json += "{";
        json += "\"ssid\":\"" + ssid + "\",";
        json += "\"rssi\":" + String(rssi) + ",";
        json += "\"encryption\":" + String(encryption);
        json += "}";
      }
      
      Serial.print("Tim thay ");
      Serial.print(n);
      Serial.println(" mang WiFi");
    } else {
      Serial.println("Khong tim thay WiFi nao");
    }
    
    json += "]";
    
    // Quay lại AP mode
    WiFi.mode(WIFI_AP);
    
    server.send(200, "application/json", json);
  });
  
  // Xử lý lưu
  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("deviceId") && 
        server.hasArg("claimCode") &&
        server.hasArg("wifiSSID") &&
        server.hasArg("wifiPassword")) {
      
      deviceId = server.arg("deviceId");
      claimCode = server.arg("claimCode");
      wifiSSID = server.arg("wifiSSID");
      wifiPassword = server.arg("wifiPassword");
      
      Serial.println("Nhan duoc cau hinh tu web:");
      Serial.print("  Device ID:   ");
      Serial.println(deviceId);
      Serial.print("  Claim Code:  ");
      Serial.println(claimCode);
      Serial.print("  WiFi SSID:   ");
      Serial.println(wifiSSID);
      Serial.println();
      
      // Lưu vào EEPROM
      saveConfiguration();
      
      // Trả về trang thành công
      String html = getSuccessHTML();
      server.send(200, "text/html", html);
      
      // Restart sau 3s
      delay(3000);
      ESP.restart();
      
    } else {
      server.send(400, "text/plain", "Missing parameters");
    }
  });
}

String getSetupHTML() {
  String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";
  html += "<title>AC Control Setup</title><style>";
  html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  html += "body { font-family: Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);";
  html += "min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 20px; }";
  html += ".container { background: white; padding: 40px; border-radius: 16px;";
  html += "box-shadow: 0 20px 60px rgba(0,0,0,0.3); max-width: 450px; width: 100%; }";
  html += "h1 { color: #667eea; margin-bottom: 10px; font-size: 28px; }";
  html += ".subtitle { color: #666; margin-bottom: 30px; font-size: 14px; }";
  html += ".form-group { margin-bottom: 20px; }";
  html += "label { display: block; margin-bottom: 8px; color: #333; font-weight: 600; font-size: 14px; }";
  html += "input { width: 100%; padding: 12px; border: 2px solid #e0e0e0; border-radius: 8px; font-size: 16px; }";
  html += "input:focus { outline: none; border-color: #667eea; }";
  html += "button { width: 100%; padding: 14px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);";
  html += "color: white; border: none; border-radius: 8px; font-size: 16px; font-weight: 600; cursor: pointer; }";
  html += "button:hover:not(:disabled) { opacity: 0.9; }";
  html += "button:disabled { opacity: 0.5; cursor: not-allowed; }";
  html += ".info { background: #f0f4ff; padding: 12px; border-radius: 8px; margin-bottom: 20px; font-size: 13px; color: #667eea; }";
  html += ".wifi-list { max-height: 250px; overflow-y: auto; border: 2px solid #e0e0e0; border-radius: 8px; margin-top: 8px; }";
  html += ".wifi-item { padding: 12px; border-bottom: 1px solid #f0f0f0; cursor: pointer; transition: background 0.2s; }";
  html += ".wifi-item:last-child { border-bottom: none; }";
  html += ".wifi-item:hover { background: #f8f9fa; }";
  html += ".wifi-item.selected { background: #e3f2fd; border-left: 4px solid #667eea; }";
  html += ".scanning { text-align: center; padding: 20px; color: #666; }";
  html += ".spinner { border: 3px solid #f3f3f3; border-top: 3px solid #667eea; border-radius: 50%;";
  html += "width: 30px; height: 30px; animation: spin 1s linear infinite; margin: 0 auto 10px; }";
  html += "@keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }";
  html += ".scan-button { background: #fff; color: #667eea; border: 2px solid #667eea; padding: 8px 16px; font-size: 14px; margin-top: 10px; }";
  html += "</style></head><body><div class=\"container\">";
  html += "<h1>AC Control</h1><p class=\"subtitle\">Cau hinh thiet bi</p>";
  html += "<div class=\"info\">Nhap Device ID va Claim Code duoc cung cap boi admin</div>";
  html += "<form id=\"setupForm\" action=\"/save\" method=\"POST\">";
  html += "<div class=\"form-group\"><label>Device ID</label>";
  html += "<input type=\"text\" name=\"deviceId\" id=\"deviceId\" placeholder=\"device_001\" required></div>";
  html += "<div class=\"form-group\"><label>Claim Code</label>";
  html += "<input type=\"text\" name=\"claimCode\" id=\"claimCode\" placeholder=\"ABC123\" required></div>";
  html += "<div class=\"form-group\"><label>Chon WiFi</label><div id=\"wifiList\" class=\"wifi-list\">";
  html += "<div class=\"scanning\"><div class=\"spinner\"></div><div>Dang quet WiFi...</div></div></div>";
  html += "<button type=\"button\" class=\"scan-button\" onclick=\"scanWifi()\">Quet lai</button>";
  html += "<input type=\"hidden\" name=\"wifiSSID\" id=\"wifiSSID\" required></div>";
  html += "<div class=\"form-group\" id=\"passwordGroup\" style=\"display: none;\">";
  html += "<label>Mat khau WiFi</label>";
  html += "<input type=\"password\" name=\"wifiPassword\" id=\"wifiPassword\" placeholder=\"Nhap mat khau\"></div>";
  html += "<button type=\"submit\" id=\"submitBtn\" disabled>LUU VA KHOI DONG LAI</button></form></div>";
  html += "<script>";
  html += "window.onload = function() { scanWifi(); };";
  html += "function scanWifi() {";
  html += "document.getElementById('wifiList').innerHTML = '<div class=\"scanning\"><div class=\"spinner\"></div><div>Dang quet WiFi...</div></div>';";
  html += "fetch('/scan').then(response => response.json()).then(networks => { displayNetworks(networks); })";
  html += ".catch(error => { document.getElementById('wifiList').innerHTML = '<div class=\"scanning\"><div style=\"color: #f44336;\">Loi quet WiFi</div></div>'; });";
  html += "}";
  html += "function displayNetworks(networks) {";
  html += "const wifiList = document.getElementById('wifiList');";
  html += "if (networks.length === 0) { wifiList.innerHTML = '<div class=\"scanning\"><div>Khong tim thay WiFi nao</div></div>'; return; }";
  html += "let h = '';";
  html += "networks.forEach(network => {";
  html += "const isEncrypted = network.encryption !== 0;";
  html += "const lockIcon = isEncrypted ? '&#128274; ' : '';";
  html += "h += '<div class=\"wifi-item\" onclick=\"selectWifi(\\'' + escapeHtml(network.ssid) + '\\', ' + isEncrypted + ')\">' + lockIcon + escapeHtml(network.ssid) + '</div>';";
  html += "});";
  html += "wifiList.innerHTML = h;";
  html += "}";
  html += "function selectWifi(ssid, isEncrypted) {";
  html += "document.getElementById('wifiSSID').value = ssid;";
  html += "document.querySelectorAll('.wifi-item').forEach(item => { item.classList.remove('selected'); });";
  html += "event.currentTarget.classList.add('selected');";
  html += "const passwordGroup = document.getElementById('passwordGroup');";
  html += "const passwordInput = document.getElementById('wifiPassword');";
  html += "if (isEncrypted) { passwordGroup.style.display = 'block'; passwordInput.required = true; }";
  html += "else { passwordGroup.style.display = 'none'; passwordInput.required = false; passwordInput.value = ''; }";
  html += "checkFormValid();";
  html += "}";
  html += "function checkFormValid() {";
  html += "const deviceId = document.getElementById('deviceId').value;";
  html += "const claimCode = document.getElementById('claimCode').value;";
  html += "const wifiSSID = document.getElementById('wifiSSID').value;";
  html += "const passwordInput = document.getElementById('wifiPassword');";
  html += "const password = passwordInput.value;";
  html += "const isValid = deviceId && claimCode && wifiSSID && (!passwordInput.required || password);";
  html += "document.getElementById('submitBtn').disabled = !isValid;";
  html += "}";
  html += "function escapeHtml(text) {";
  html += "const div = document.createElement('div');";
  html += "div.textContent = text;";
  html += "return div.innerHTML;";
  html += "}";
  html += "document.getElementById('deviceId').addEventListener('input', checkFormValid);";
  html += "document.getElementById('claimCode').addEventListener('input', checkFormValid);";
  html += "document.getElementById('wifiPassword').addEventListener('input', checkFormValid);";
  html += "</script></body></html>";
  
  return html;
}

String getSuccessHTML() {
  String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";
  html += "<title>Thanh cong</title><style>";
  html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  html += "body { font-family: Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);";
  html += "min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 20px; }";
  html += ".container { background: white; padding: 40px; border-radius: 16px;";
  html += "box-shadow: 0 20px 60px rgba(0,0,0,0.3); max-width: 400px; width: 100%; text-align: center; }";
  html += ".success-icon { font-size: 64px; margin-bottom: 20px; }";
  html += "h1 { color: #4caf50; margin-bottom: 10px; }";
  html += "p { color: #666; line-height: 1.6; }";
  html += "</style></head><body><div class=\"container\">";
  html += "<div class=\"success-icon\">&#9989;</div>";
  html += "<h1>Thanh cong!</h1>";
  html += "<p>Thiet bi dang khoi dong lai...</p>";
  html += "<p style=\"margin-top: 20px; font-size: 14px; color: #999;\">Ban co the dong trang nay</p>";
  html += "</div></body></html>";
  
  return html;
}

// ═══════════════════════════════════════
// WIFI
// ═══════════════════════════════════════
void connectWiFi() {
  Serial.println("Dang ket noi WiFi...");
  Serial.print("SSID: ");
  Serial.println(wifiSSID);
  
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✓ WiFi da ket noi");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();
  } else {
    Serial.println("✗ Khong the ket noi WiFi");
    Serial.println("Vui long cau hinh lai");
    Serial.println();
    
    // Reset và vào setup mode
    factoryReset();
  }
}

// ═══════════════════════════════════════
// TIME SYNCHRONIZATION (FIX SSL ERROR)
// ═══════════════════════════════════════
void syncTime() {
  Serial.println("Dang dong bo thoi gian NTP...");
  Serial.println("QUAN TRONG: NTP phai thanh cong de SSL hoat dong!");

  // Configure NTP với múi giờ Việt Nam (GMT+7)
  // Sử dụng nhiều NTP server để tăng độ tin cậy
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  Serial.print("Cho NTP dong bo");
  int attempts = 0;
  time_t now = time(nullptr);

  // Đợi lâu hơn và kiểm tra chặt chẽ hơn
  while (now < 100000 && attempts < 30) {  // Tăng từ 20 → 30 lần
    delay(1000);  // Tăng từ 500ms → 1000ms
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }

  Serial.println();

  if (now > 100000) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);  // Dùng localtime_r thay vì gmtime_r để hiển thị giờ local
    Serial.println("✓✓✓ DONG BO THOI GIAN THANH CONG!");
    Serial.print("Thoi gian hien tai (GMT+7): ");
    Serial.println(asctime(&timeinfo));
    Serial.print("Unix timestamp: ");
    Serial.println(now);
    Serial.println();
  } else {
    Serial.println("✗✗✗ KHONG THE DONG BO THOI GIAN!");
    Serial.println();
    Serial.println("Nguyen nhan co the:");
    Serial.println("- Router chan NTP port (UDP 123)");
    Serial.println("- WiFi khong on dinh");
    Serial.println("- ISP chan NTP servers");
    Serial.println();
    Serial.println("SSL SE KHONG HOAT DONG!");
    Serial.println("Thiet bi se khoi dong lai sau 10 giay...");
    delay(10000);
    ESP.restart();
  }
}

// ═══════════════════════════════════════
// FIREBASE
// ═══════════════════════════════════════
void connectFirebase() {
  Serial.println("Dang ket noi Firebase...");

  // In thông tin heap trước khi kết nối
  Serial.print("Free heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  firebaseConfig.host = FIREBASE_HOST;
  firebaseConfig.signer.tokens.legacy_token = FIREBASE_AUTH;

  // Tăng timeout và buffer size
  firebaseConfig.timeout.serverResponse = 15 * 1000; // 15 seconds
  firebaseConfig.timeout.socketConnection = 15 * 1000;
  firebaseConfig.timeout.sslHandshake = 60 * 1000; // 60 seconds for SSL
  firebaseConfig.timeout.rtdbKeepAlive = 45 * 1000;
  firebaseConfig.timeout.rtdbStreamReconnect = 1 * 1000;
  firebaseConfig.timeout.rtdbStreamError = 3 * 1000;

  // Disable WiFi sleep để tránh mất kết nối
  WiFi.setSleep(false);

  Firebase.begin(&firebaseConfig, &firebaseAuth);
  Firebase.reconnectWiFi(true);

  // Chờ một chút để Firebase khởi tạo
  delay(2000);

  // Kiểm tra kết nối bằng cách thử ghi dữ liệu đơn giản
  Serial.println("Kiem tra ket noi Firebase...");
  int retryCount = 0;
  bool connected = false;

  while (!connected && retryCount < 5) {
    String testPath = "devices/" + deviceId + "/info/macAddress";
    String macAddress = WiFi.macAddress();

    if (Firebase.setString(firebaseData, testPath, macAddress)) {
      Serial.println("✓ Firebase da ket noi thanh cong!");
      connected = true;
    } else {
      Serial.print("✗ Lan thu ");
      Serial.print(retryCount + 1);
      Serial.print(" that bai: ");
      Serial.println(firebaseData.errorReason());

      retryCount++;
      delay(2000);
    }
  }

  if (!connected) {
    Serial.println("✗✗✗ KHONG THE KET NOI FIREBASE!");
    Serial.println("Thiet bi se khoi dong lai sau 10 giay...");
    delay(10000);
    ESP.restart();
    return;
  }

  // Cập nhật online status
  String path = "status/" + deviceId + "/online";
  Firebase.setBool(firebaseData, path, true);

  Serial.println("✓ Firebase hoan toan san sang");
  Serial.println();
  
  // Setup stream để lắng nghe lệnh
  String streamPath = "commands/" + deviceId;
  if (!Firebase.beginStream(streamData, streamPath)) {
    Serial.println("✗ Loi khoi tao Firebase Stream");
    Serial.println(streamData.errorReason());
  } else {
    Serial.println("✓ Firebase Stream da san sang");
    Serial.print("Dang lang nghe: ");
    Serial.println(streamPath);
  }
  
  Serial.println();
}
// ═══════════════════════════════════════
// CLAIM DEVICE (KIỂM TRA BẢO MẬT)
// ═══════════════════════════════════════
void claimDevice() {
  Serial.println();
  Serial.println("═════════════════════════════════════");
  Serial.println("  KIEM TRA VA CLAIM DEVICE");
  Serial.println("═════════════════════════════════════");
  
  Serial.print("Device ID: ");
  Serial.println(deviceId);
  Serial.print("Claim Code: ");
  Serial.println(claimCode);
  Serial.println();
  
  // ===== BƯỚC 1: KIỂM TRA DEVICE CÓ TỒN TẠI KHÔNG =====
  Serial.println(">>> [1/5] Kiem tra device trong database...");
  
  String checkPath = "devices/" + deviceId + "/info/deviceId";
  if (!Firebase.getString(firebaseData, checkPath)) {
    Serial.println("✗✗✗ LOI: DEVICE KHONG TON TAI TRONG HE THONG!");
    Serial.println();
    Serial.println("Nguyen nhan:");
    Serial.println("- Admin chua tao device nay trong admin tool");
    Serial.println("- Device ID sai");
    Serial.println();
    Serial.println("Giai phap:");
    Serial.println("1. Kiem tra lai Device ID tren nhan thiet bi");
    Serial.println("2. Lien he admin de them device vao he thong");
    Serial.println("═════════════════════════════════════");
    
    // Nhấp nháy LED nhanh 10 lần = lỗi
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, HIGH);
      delay(100);
      digitalWrite(LED_PIN, LOW);
      delay(100);
    }
    
    // Quay lại setup mode
    delay(5000);
    factoryReset();
    return;
  }
  
  Serial.println("✓ Device ton tai trong database");
  Serial.println();
  
  // ===== BƯỚC 2: KIỂM TRA PUBLISHED =====
  Serial.println(">>> [2/5] Kiem tra device da duoc published...");
  
  bool published = false;
  String publishedPath = "devices/" + deviceId + "/info/published";
  
  if (Firebase.getBool(firebaseData, publishedPath)) {
    published = firebaseData.boolData();
  }
  
  if (!published) {
    Serial.println("✗✗✗ LOI: DEVICE CHUA DUOC KICH HOAT!");
    Serial.println();
    Serial.println("Nguyen nhan:");
    Serial.println("- Admin chua kich hoat device (published = false)");
    Serial.println();
    Serial.println("Giai phap:");
    Serial.println("- Lien he admin de kich hoat device trong admin tool");
    Serial.println("═════════════════════════════════════");
    
    // Nhấp nháy LED chậm 5 lần
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_PIN, HIGH);
      delay(500);
      digitalWrite(LED_PIN, LOW);
      delay(500);
    }
    
    delay(5000);
    factoryReset();
    return;
  }
  
  Serial.println("✓ Device da duoc published");
  Serial.println();
  
  // ===== BƯỚC 3: KIỂM TRA CLAIM CODE =====
  Serial.println(">>> [3/5] Kiem tra claim code...");
  
  String correctClaimCode = "";
  String claimCodePath = "devices/" + deviceId + "/info/claimCode";
  
  if (Firebase.getString(firebaseData, claimCodePath)) {
    correctClaimCode = firebaseData.stringData();
  }
  
  if (correctClaimCode == "") {
    Serial.println("✗✗✗ LOI: Device khong co claim code!");
    Serial.println("Lien he admin.");
    Serial.println("═════════════════════════════════════");
    
    delay(5000);
    factoryReset();
    return;
  }
  
  if (correctClaimCode != claimCode) {
    Serial.println("✗✗✗ LOI: CLAIM CODE SAI!");
    Serial.println();
    Serial.print("Claim code dung:      ");
    Serial.println(correctClaimCode);
    Serial.print("Claim code ban nhap:  ");
    Serial.println(claimCode);
    Serial.println();
    Serial.println("Giai phap:");
    Serial.println("- Kiem tra lai claim code tren nhan thiet bi");
    Serial.println("- Config lai tu dau voi claim code dung");
    Serial.println("═════════════════════════════════════");
    
    // Nhấp nháy LED nhanh 10 lần
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, HIGH);
      delay(100);
      digitalWrite(LED_PIN, LOW);
      delay(100);
    }
    
    delay(5000);
    factoryReset();
    return;
  }
  
  Serial.println("✓ Claim code dung");
  Serial.println();
  
  // ===== BƯỚC 4: KIỂM TRA ĐÃ ĐƯỢC CLAIM CHƯA =====
  Serial.println(">>> [4/5] Kiem tra trang thai claim...");
  
  bool alreadyClaimed = false;
  String claimedPath = "devices/" + deviceId + "/info/claimed";
  
  if (Firebase.getBool(firebaseData, claimedPath)) {
    alreadyClaimed = firebaseData.boolData();
  }
  
  if (alreadyClaimed) {
    Serial.println("⚠️ Device da duoc claim truoc do");
    
    // Kiểm tra MAC address để xem có phải device này không
    String savedMac = "";
    String macPath = "devices/" + deviceId + "/info/macAddress";
    
    if (Firebase.getString(firebaseData, macPath)) {
      savedMac = firebaseData.stringData();
    }
    
    String currentMac = WiFi.macAddress();
    
    if (savedMac == currentMac) {
      Serial.println("✓ Day la device cu (MAC address khop)");
      Serial.println("Tiep tuc ket noi...");
    } else {
      Serial.println("✗✗✗ LOI: Device da thuoc ve nguoi khac!");
      Serial.println();
      Serial.print("MAC da luu:    ");
      Serial.println(savedMac);
      Serial.print("MAC hien tai:  ");
      Serial.println(currentMac);
      Serial.println();
      Serial.println("Giai phap:");
      Serial.println("- Lien he owner cu de xoa device");
      Serial.println("- Hoac lien he admin de reset device");
      Serial.println("═════════════════════════════════════");
      
      for (int i = 0; i < 10; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
      }
      
      delay(10000);
      factoryReset();
      return;
    }
  } else {
    Serial.println("✓ Device chua duoc claim, se claim ngay bay gio");
  }
  
  Serial.println();
  
  // ===== BƯỚC 5: CẬP NHẬT THÔNG TIN ESP32 =====
  Serial.println(">>> [5/5] Cap nhat thong tin ESP32 len Firebase...");

  String macAddress = WiFi.macAddress();
  String basePath = "devices/" + deviceId + "/info";

  // Kiểm tra MAC address trên Firebase để xác định có phải lần đầu claim không
  String savedMacAddress = "";
  if (Firebase.getString(firebaseData, basePath + "/macAddress")) {
    savedMacAddress = firebaseData.stringData();
  }

  // Chỉ claim khi MAC hiện tại khớp với MAC đã lưu (device cũ)
  // HOẶC chưa bao giờ có MAC (device mới)
  if (alreadyClaimed && savedMacAddress == macAddress) {
    // Device cũ đang reconnect → CHỈ cập nhật firmware
    Firebase.setString(firebaseData, basePath + "/firmwareVersion", FIRMWARE_VERSION);
    Serial.println("✓ Da cap nhat firmware version (device cu)");
  } else if (!alreadyClaimed && savedMacAddress == "") {
    // Device mới (chưa bao giờ claim) → Claim ngay
    Firebase.setString(firebaseData, basePath + "/macAddress", macAddress);
    Firebase.setString(firebaseData, basePath + "/firmwareVersion", FIRMWARE_VERSION);
    Firebase.setBool(firebaseData, basePath + "/claimed", true);
    Serial.println("✓ Da claim device moi");
  } else {
    // Device đã reset (claimed = false, macAddress = "") → KHÔNG claim
    Firebase.setString(firebaseData, basePath + "/firmwareVersion", FIRMWARE_VERSION);
    Serial.println("✓ Cap nhat firmware (device da reset, khong claim lai)");
  }

  // Luôn cập nhật lastModified
  Firebase.setInt(firebaseData, basePath + "/lastModified", millis());

  Serial.println("✓ Da cap nhat thong tin ESP32");
  Serial.println();
  
  // Cập nhật status
  Firebase.setBool(firebaseData, "status/" + deviceId + "/online", true);
  Firebase.setInt(firebaseData, "status/" + deviceId + "/lastSeen", millis());
  
  Serial.println("✓✓✓ HOAN THANH CLAIM DEVICE!");
  Serial.println("Device da san sang hoat dong.");
  Serial.println("═════════════════════════════════════");
  Serial.println();
  
  // LED sáng liên tục = thành công
  digitalWrite(LED_PIN, HIGH);
}
// ═══════════════════════════════════════
// RESET DHT22 KHI BỊ TREO
// ═══════════════════════════════════════
void resetDHT22() {
  Serial.println("⚠️ DHT22 bi treo - Dang reset...");

  // Tắt nguồn DHT bằng cách set pin thành OUTPUT LOW
  pinMode(DHT_PIN, OUTPUT);
  digitalWrite(DHT_PIN, LOW);
  delay(500); // Chờ DHT discharge hoàn toàn

  // Bật lại DHT
  pinMode(DHT_PIN, INPUT_PULLUP);
  delay(100);

  // Khởi tạo lại thư viện DHT
  dht.begin();
  delay(2000); // DHT22 cần 1-2 giây để ổn định

  Serial.println("✓ Da reset DHT22 thanh cong");
  dhtErrorCount = 0; // Reset counter
}

// ═══════════════════════════════════════
// RESET PZEM KHI BỊ TREO
// ═══════════════════════════════════════
void resetPZEM() {
  Serial.println("⚠️ PZEM-004T bi treo - Dang reset...");
  Serial.println("========================================");
  Serial.println("THONG TIN TRUOC KHI RESET:");
  Serial.print("  Last valid voltage: ");
  Serial.print(lastValidVoltage);
  Serial.println(" V");
  Serial.print("  Last valid current: ");
  Serial.print(lastValidCurrent);
  Serial.println(" A");
  Serial.print("  Last valid power: ");
  Serial.print(lastValidPower);
  Serial.println(" W");
  Serial.println("========================================");

  // Dừng serial cũ
  pzemSerial.end();
  delay(500);

  // Khởi tạo lại serial và PZEM
  pzemSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
  delay(1000);

  // Reset địa chỉ PZEM
  pzem.resetEnergy();

  Serial.println("✓ Da reset PZEM-004T thanh cong");
  Serial.println("Cho PZEM on dinh...");
  delay(2000);

  // Test đọc lại
  float testVoltage = pzem.voltage();
  if (!isnan(testVoltage)) {
    Serial.print("✓ PZEM hoat dong lai - Voltage: ");
    Serial.print(testVoltage);
    Serial.println(" V");

    // Reset tất cả counters
    pzemErrorCount = 0;
    pzemStuckCount = 0;
  } else {
    Serial.println("✗ PZEM van chua hoat dong");
  }

  Serial.println("========================================");
  Serial.println();
}

// ═══════════════════════════════════════
// ĐỌC CẢM BIẾN (Dựa trên code test)
// ═══════════════════════════════════════
void readSensors() {
  unsigned long currentMillis = millis();
  
  // Đọc mỗi 2 giây
  if (currentMillis - lastSensorRead >= 2000) {
    lastSensorRead = currentMillis;
    
    // DHT22 - Đọc với khả năng tự động phục hồi
    roomTemp = dht.readTemperature();
    roomHumidity = dht.readHumidity();

    if (isnan(roomTemp) || isnan(roomHumidity)) {
      Serial.println("✗ Loi doc DHT22");
      dhtErrorCount++;

      // Nếu lỗi liên tục 2 lần → Reset DHT22
      if (dhtErrorCount >= DHT_MAX_ERRORS) {
        resetDHT22();
        // Đọc lại sau khi reset
        roomTemp = dht.readTemperature();
        roomHumidity = dht.readHumidity();

        if (!isnan(roomTemp) && !isnan(roomHumidity)) {
          Serial.println("✓ DHT22 hoat dong lai sau khi reset");
        } else {
          roomTemp = 0;
          roomHumidity = 0;
        }
      } else {
        roomTemp = 0;
        roomHumidity = 0;
      }
    } else {
      // Đọc thành công → Reset error counter
      dhtErrorCount = 0;
    }
    
    // HLK-LD2410C
    personDetected = digitalRead(HLK_OUT_PIN) == HIGH;
    
    // PZEM-004T với khả năng tự động phục hồi
    float voltage = pzem.voltage();

    if (!isnan(voltage) && voltage > 0) {
      // Đọc thành công
      powerCurrent = pzem.current();
      powerWatt = pzem.power();

      // ========================================
      // KIỂM TRA GIÁ TRỊ BỊ ĐỨNG (STUCK)
      // ========================================
      // Chỉ kiểm tra stuck khi đã có giá trị cũ (không phải lần đầu)
      if (lastValidVoltage > 0 && powerWatt > 100) {
        // So sánh với giá trị lần đọc trước
        float voltageDiff = abs(voltage - lastValidVoltage);
        float currentDiff = abs(powerCurrent - lastValidCurrent);
        float powerDiff = abs(powerWatt - lastValidPower);

        // Debug: In ra để kiểm tra
        Serial.print("[STUCK CHECK] V_diff=");
        Serial.print(voltageDiff, 3);
        Serial.print(" A_diff=");
        Serial.print(currentDiff, 3);
        Serial.print(" W_diff=");
        Serial.print(powerDiff, 2);
        Serial.print(" count=");
        Serial.println(pzemStuckCount);

        // Nếu tất cả giá trị gần như không đổi
        if (voltageDiff < 0.1 && currentDiff < 0.01 && powerDiff < 1.0) {
          pzemStuckCount++;
          Serial.print("⚠️ GIA TRI STUCK (");
          Serial.print(pzemStuckCount);
          Serial.print("/");
          Serial.print(PZEM_STUCK_THRESHOLD);
          Serial.println(")");

          if (pzemStuckCount >= PZEM_STUCK_THRESHOLD) {
            Serial.println();
            Serial.println("⚠️⚠️⚠️ PHAT HIEN PZEM BI STUCK!");
            Serial.print("Gia tri dung im: V=");
            Serial.print(voltage, 2);
            Serial.print(" A=");
            Serial.print(powerCurrent, 2);
            Serial.print(" W=");
            Serial.println(powerWatt, 2);
            Serial.println();

            // Force reset PZEM
            resetPZEM();
            pzemStuckCount = 0;

            // Đọc lại
            voltage = pzem.voltage();
            powerCurrent = pzem.current();
            powerWatt = pzem.power();
          }
        } else {
          // Giá trị có thay đổi → reset stuck counter
          if (pzemStuckCount > 0) {
            Serial.println("✓ Gia tri thay doi - reset stuck counter");
          }
          pzemStuckCount = 0;
        }
      }

      // Lưu giá trị hợp lệ cuối cùng
      lastValidVoltage = voltage;
      if (!isnan(powerCurrent)) lastValidCurrent = powerCurrent;
      if (!isnan(powerWatt)) lastValidPower = powerWatt;

      // Reset error counter khi đọc thành công
      pzemErrorCount = 0;

      // ========================================
      // LOGIC: Tự động phát hiện trạng thái AC
      // ========================================
      bool detectedPowerOn = powerCurrent > 0.2;  // Ngưỡng 0.2A

      // Nếu phát hiện khác với trạng thái hiện tại
      if (detectedPowerOn != acPowerOn) {
        acPowerOn = detectedPowerOn;

        Serial.println("========================================");
        Serial.println("TU DONG CAP NHAT TRANG THAI AC!");
        Serial.print("Dong dien: ");
        Serial.print(powerCurrent);
        Serial.println(" A");
        Serial.print("Trang thai moi: ");
        Serial.println(acPowerOn ? "BAT" : "TAT");
        Serial.println("========================================");
      }

    } else {
      // Đọc thất bại
      Serial.println("✗ Loi doc PZEM-004T");
      pzemErrorCount++;

      // Nếu lỗi liên tục → Reset PZEM
      if (pzemErrorCount >= PZEM_MAX_ERRORS) {
        resetPZEM();

        // Đọc lại sau khi reset
        voltage = pzem.voltage();
        if (!isnan(voltage) && voltage > 0) {
          powerCurrent = pzem.current();
          powerWatt = pzem.power();

          lastValidVoltage = voltage;
          if (!isnan(powerCurrent)) lastValidCurrent = powerCurrent;
          if (!isnan(powerWatt)) lastValidPower = powerWatt;

          Serial.println("✓ PZEM hoat dong lai sau khi reset");
        } else {
          // Vẫn lỗi → Dùng giá trị cũ
          Serial.println("⚠️ PZEM van loi - Su dung gia tri cu");
          powerCurrent = lastValidCurrent;
          powerWatt = lastValidPower;
        }
      } else {
        // Chưa đủ số lần lỗi → Dùng giá trị cũ
        powerCurrent = lastValidCurrent;
        powerWatt = lastValidPower;
      }

      // Không đọc được PZEM → coi như AC tắt
      if (acPowerOn && pzemErrorCount >= PZEM_MAX_ERRORS) {
        acPowerOn = false;
        Serial.println("⚠️ Mat tin hieu PZEM - Dat AC = TAT");
      }
    }
    
    // Debug
    Serial.println("--- Cam bien ---");
    Serial.print("Nhiet do: ");
    Serial.print(roomTemp);
    Serial.println(" *C");
    Serial.print("Do am: ");
    Serial.print(roomHumidity);
    Serial.println(" %");
    Serial.print("Co nguoi: ");
    Serial.println(personDetected ? "Co" : "Khong");
    Serial.print("Dong dien: ");
    Serial.print(powerCurrent);
    Serial.println(" A");
    Serial.print("Cong suat: ");
    Serial.print(powerWatt);
    Serial.println(" W");
    //Serial.print("Trang thai AC: ");
    //Serial.println(acPowerOn ? "BAT" : "TAT");
    //Serial.println("----------------");
  }
}

// ═══════════════════════════════════════
// CẬP NHẬT STATUS LÊN FIREBASE
// ═══════════════════════════════════════
void updateStatus() {
  unsigned long currentMillis = millis();
  
  // Update mỗi 5 giây
  if (currentMillis - lastStatusUpdate >= 5000) {
    lastStatusUpdate = currentMillis;
    
    String basePath = "status/" + deviceId;
    
    // Room status
    Firebase.setFloat(firebaseData, basePath + "/room/temp", roomTemp);
    Firebase.setFloat(firebaseData, basePath + "/room/humidity", roomHumidity);
    Firebase.setBool(firebaseData, basePath + "/room/hasPerson", personDetected);
    Firebase.setInt(firebaseData, basePath + "/room/lastUpdate", millis());
    
    // AC status
    Firebase.setBool(firebaseData, basePath + "/ac/powerOn", acPowerOn);
    Firebase.setInt(firebaseData, basePath + "/ac/setTemp", currentTemp);
    Firebase.setFloat(firebaseData, basePath + "/ac/current", powerCurrent);
    Firebase.setFloat(firebaseData, basePath + "/ac/power", powerWatt);
    Firebase.setInt(firebaseData, basePath + "/ac/lastUpdate", millis());
  }
}

// ═══════════════════════════════════════
// GỬI HEARTBEAT
// ═══════════════════════════════════════
void sendHeartbeat() {
  unsigned long currentMillis = millis();
  
  // Heartbeat mỗi 10 giây
  if (currentMillis - lastHeartbeat >= 10000) {
    lastHeartbeat = currentMillis;
    
    String path = "status/" + deviceId + "/online";
    Firebase.setBool(firebaseData, path, true);
    
    path = "status/" + deviceId + "/lastSeen";
    Firebase.setInt(firebaseData, path, millis());
  }
}

// ═══════════════════════════════════════
// KHỞI TẠO IR AC THEO PROTOCOL
// ═══════════════════════════════════════
void initializeIRProtocol(String protocol) {
  currentProtocol = protocol;
  
  Serial.println("Khoi tao IR AC:");
  Serial.print("  Protocol: ");
  Serial.println(protocol);
  
  // Giải phóng memory cũ
  if (daikinAC) { delete daikinAC; daikinAC = nullptr; }
  if (panasonicAC) { delete panasonicAC; panasonicAC = nullptr; }
  if (mitsubishiAC) { delete mitsubishiAC; mitsubishiAC = nullptr; }
  if (lgAC) { delete lgAC; lgAC = nullptr; }
  if (samsungAC) { delete samsungAC; samsungAC = nullptr; }
  if (toshibaAC) { delete toshibaAC; toshibaAC = nullptr; }
  
  // Khởi tạo AC object mới
  if (protocol == "DAIKIN") {
    daikinAC = new IRDaikinESP(IR_SEND_PIN);
    daikinAC->begin();
    Serial.println("  ✓ Daikin initialized");
    
  } else if (protocol == "PANASONIC_AC") {
    panasonicAC = new IRPanasonicAc(IR_SEND_PIN);
    panasonicAC->begin();
    Serial.println("  ✓ Panasonic initialized");
    
  } else if (protocol == "MITSUBISHI_AC") {
    mitsubishiAC = new IRMitsubishiAC(IR_SEND_PIN);
    mitsubishiAC->begin();
    Serial.println("  ✓ Mitsubishi initialized");
    
  } else if (protocol == "LG") {
    lgAC = new IRLgAc(IR_SEND_PIN);
    lgAC->begin();
    Serial.println("  ✓ LG initialized");
    
  } else if (protocol == "SAMSUNG_AC") {
    samsungAC = new IRSamsungAc(IR_SEND_PIN);
    samsungAC->begin();
    Serial.println("  ✓ Samsung initialized");
    
  } else if (protocol == "TOSHIBA_AC") {
    toshibaAC = new IRToshibaAC(IR_SEND_PIN);
    toshibaAC->begin();
    Serial.println("  ✓ Toshiba initialized");
    
  } else {
    Serial.println("  ✗ Unknown protocol");
  }
  
  Serial.println();
}

// ═══════════════════════════════════════
// ĐIỀU KHIỂN IR - DÙNG THƯ VIỆN
// ═══════════════════════════════════════
void sendIRLibraryCommand(String action) {
  Serial.print("GUI LENH IR (Library): ");
  Serial.println(action);
  
  if (currentProtocol == "DAIKIN" && daikinAC) {
    if (action == "power") {
      if (acPowerOn) {
        daikinAC->off();
        acPowerOn = false;
      } else {
        daikinAC->on();
        daikinAC->setTemp(currentTemp);
        daikinAC->setMode(kDaikinCool);
        daikinAC->setFan(kDaikinFanAuto);
        acPowerOn = true;
      }
    } else if (action == "tempUp" && acPowerOn) {
      if (currentTemp < 30) {
        currentTemp++;
        daikinAC->setTemp(currentTemp);
      }
    } else if (action == "tempDown" && acPowerOn) {
      if (currentTemp > 16) {
        currentTemp--;
        daikinAC->setTemp(currentTemp);
      }
    } else if (action == "setTemp" && acPowerOn) {
      daikinAC->setTemp(currentTemp);
    }
    daikinAC->send();
    
  } else if (currentProtocol == "PANASONIC_AC" && panasonicAC) {
    if (action == "power") {
      if (acPowerOn) {
        panasonicAC->off();
        acPowerOn = false;
      } else {
        panasonicAC->on();
        panasonicAC->setTemp(currentTemp);
        panasonicAC->setMode(kPanasonicAcCool);
        panasonicAC->setFan(kPanasonicAcFanAuto);
        acPowerOn = true;
      }
    } else if (action == "tempUp" && acPowerOn) {
      if (currentTemp < 30) {
        currentTemp++;
        panasonicAC->setTemp(currentTemp);
      }
    } else if (action == "tempDown" && acPowerOn) {
      if (currentTemp > 16) {
        currentTemp--;
        panasonicAC->setTemp(currentTemp);
      }
    } else if (action == "setTemp" && acPowerOn) {
      panasonicAC->setTemp(currentTemp);
    }
    panasonicAC->send();
    
  } else if (currentProtocol == "MITSUBISHI_AC" && mitsubishiAC) {
    if (action == "power") {
      if (acPowerOn) {
        mitsubishiAC->off();
        acPowerOn = false;
      } else {
        mitsubishiAC->on();
        mitsubishiAC->setTemp(currentTemp);
        mitsubishiAC->setMode(kMitsubishiAcCool);
        mitsubishiAC->setFan(kMitsubishiAcFanAuto);
        acPowerOn = true;
      }
    } else if (action == "tempUp" && acPowerOn) {
      if (currentTemp < 30) {
        currentTemp++;
        mitsubishiAC->setTemp(currentTemp);
      }
    } else if (action == "tempDown" && acPowerOn) {
      if (currentTemp > 16) {
        currentTemp--;
        mitsubishiAC->setTemp(currentTemp);
      }
    } else if (action == "setTemp" && acPowerOn) {
      mitsubishiAC->setTemp(currentTemp);
    }
    mitsubishiAC->send();
    
  } else if (currentProtocol == "LG" && lgAC) {
    if (action == "power") {
      if (acPowerOn) {
        lgAC->off();
        acPowerOn = false;
      } else {
        lgAC->on();
        lgAC->setTemp(currentTemp);
        lgAC->setMode(kLgAcCool);
        lgAC->setFan(kLgAcFanAuto);
        acPowerOn = true;
      }
    } else if (action == "tempUp" && acPowerOn) {
      if (currentTemp < 30) {
        currentTemp++;
        lgAC->setTemp(currentTemp);
      }
    } else if (action == "tempDown" && acPowerOn) {
      if (currentTemp > 16) {
        currentTemp--;
        lgAC->setTemp(currentTemp);
      }
    } else if (action == "setTemp" && acPowerOn) {
      lgAC->setTemp(currentTemp);
    }
    lgAC->send();
    
  } else if (currentProtocol == "SAMSUNG_AC" && samsungAC) {
    if (action == "power") {
      if (acPowerOn) {
        samsungAC->off();
        acPowerOn = false;
      } else {
        samsungAC->on();
        samsungAC->setTemp(currentTemp);
        samsungAC->setMode(kSamsungAcCool);
        samsungAC->setFan(kSamsungAcFanAuto);
        acPowerOn = true;
      }
    } else if (action == "tempUp" && acPowerOn) {
      if (currentTemp < 30) {
        currentTemp++;
        samsungAC->setTemp(currentTemp);
      }
    } else if (action == "tempDown" && acPowerOn) {
      if (currentTemp > 16) {
        currentTemp--;
        samsungAC->setTemp(currentTemp);
      }
    } else if (action == "setTemp" && acPowerOn) {
      samsungAC->setTemp(currentTemp);
    }
    samsungAC->send();
    
  } else if (currentProtocol == "TOSHIBA_AC" && toshibaAC) {
    if (action == "power") {
      if (acPowerOn) {
        toshibaAC->off();
        acPowerOn = false;
      } else {
        toshibaAC->on();
        toshibaAC->setTemp(currentTemp);
        toshibaAC->setMode(kToshibaAcCool);
        acPowerOn = true;
      }
    } else if (action == "tempUp" && acPowerOn) {
      if (currentTemp < 30) {
        currentTemp++;
        toshibaAC->setTemp(currentTemp);
      }
    } else if (action == "tempDown" && acPowerOn) {
      if (currentTemp > 16) {
        currentTemp--;
        toshibaAC->setTemp(currentTemp);
      }
    } else if (action == "setTemp" && acPowerOn) {
      toshibaAC->setTemp(currentTemp);
    }
    toshibaAC->send();
  }
  
  Serial.println("  ✓ Da gui tin hieu IR");
}

// ═══════════════════════════════════════
// ĐIỀU KHIỂN IR - DÙNG LỆNH ĐÃ HỌC
// ═══════════════════════════════════════
void sendIRLearnedCommand(String action) {
  Serial.print("GUI LENH IR (Learned): ");
  Serial.println(action);

  // Tắt receiver khi phát
  irrecv.disableIRIn();
  delay(50);

  if (action == "power") {
    // Quyết định BẬT hay TẮT dựa trên trạng thái hiện tại
    if (acPowerOn) {
      // AC đang BẬT → Gửi lệnh TẮT
      if (learnedPowerOffLen > 0) {
        Serial.print("  Phat POWER_OFF (");
        Serial.print(learnedPowerOffLen);
        Serial.println(" pulses)");
        irsend.sendRaw(learnedPowerOff, learnedPowerOffLen, irFrequency);
        delay(200);
        
        acPowerOn = false;
      } else {
        Serial.println("  ✗ Chua hoc lenh POWER_OFF");
      }
    } else {
      // AC đang TẮT → Gửi lệnh BẬT
      if (learnedPowerOnLen > 0) {
        Serial.print("  Phat POWER_ON (");
        Serial.print(learnedPowerOnLen);
        Serial.println(" pulses)");
        irsend.sendRaw(learnedPowerOn, learnedPowerOnLen, irFrequency);
        delay(200);
        acPowerOn = true;
      } else {
        Serial.println("  ✗ Chua hoc lenh POWER_ON");
      }
    }

  } else if (action == "tempUp" && learnedTempUpLen > 0) {
    Serial.print("  Phat TEMP_UP (");
    Serial.print(learnedTempUpLen);
    Serial.println(" pulses)");
      irsend.sendRaw(learnedTempUp, learnedTempUpLen, irFrequency);
      delay(200);
    if (currentTemp < 30) currentTemp++;

  } else if (action == "tempDown" && learnedTempDownLen > 0) {
    Serial.print("  Phat TEMP_DOWN (");
    Serial.print(learnedTempDownLen);
    Serial.println(" pulses)");
    irsend.sendRaw(learnedTempDown, learnedTempDownLen, irFrequency);
    delay(200);
    if (currentTemp > 16) currentTemp--;

  } else {
    Serial.println("  ✗ Chua hoc lenh nay");
  }

  Serial.println("  ✓ Da gui tin hieu IR");

  delay(100);

  // Bật lại receiver
  irrecv.enableIRIn();
}

// ═══════════════════════════════════════
// KIỂM TRA VÀ XỬ LÝ LỆNH TỪ FIREBASE
// ═══════════════════════════════════════
void checkCommands() {
  // Kiểm tra stream có data mới không
  if (!Firebase.readStream(streamData)) {
    return;
  }
  
  if (streamData.streamAvailable()) {
    Serial.println("═════════════════════════════════════");
    Serial.println("NHAN DUOC LENH TU FIREBASE");
    Serial.println("═════════════════════════════════════");
    
    // Lấy toàn bộ data từ commands/device_001
    String basePath = "commands/" + deviceId;
    
    // Đọc action
    String actionPath = basePath + "/action";
    String action = "";
    
    if (Firebase.getString(firebaseData, actionPath)) {
      action = firebaseData.stringData();
    }
    
    if (action == "" || action == "null") {
      Serial.println("Lenh rong, bo qua");
      Serial.println("═════════════════════════════════════");
      Serial.println();
      return;
    }
    
    Serial.print("Action: ");
    Serial.println(action);
    
    // Lấy value (nếu có)
    int value = 0;
    String valuePath = basePath + "/value";
    if (Firebase.getInt(firebaseData, valuePath)) {
      value = firebaseData.intData();
    }
    
    // Xử lý lệnh đặc biệt
    if (action == "reset") {
      Serial.println("NHAN LENH FACTORY RESET!");
      
      // Xóa lệnh trước khi reset
      Firebase.deleteNode(firebaseData, basePath);
      delay(500);
      
      factoryReset();
      return;
    }
    
    if (action == "setTemp") {
      if (value >= 16 && value <= 30) {
        currentTemp = value;
        Serial.print("Set nhiet do: ");
        Serial.print(currentTemp);
        Serial.println("*C");
      }
    }
    
    // Kiểm tra phương thức IR
    String irMethod = "";
    String irMethodPath = "devices/" + deviceId + "/ir/method";
    if (Firebase.getString(firebaseData, irMethodPath)) {
      irMethod = firebaseData.stringData();
    }
    
    Serial.print("IR Method: ");
    Serial.println(irMethod);
    
    // Xử lý theo method
    if (irMethod == "library") {
      // Load protocol nếu chưa có
      if (currentProtocol == "") {
        String brandPath = "devices/" + deviceId + "/ir/library/brandId";
        if (Firebase.getString(firebaseData, brandPath)) {
          String brandId = firebaseData.stringData();
          
          // Load protocol từ ir_library
          String protocolPath = "ir_library/" + brandId + "/protocol";
          if (Firebase.getString(firebaseData, protocolPath)) {
            String protocol = firebaseData.stringData();
            initializeIRProtocol(protocol);
          }
        }
      }
      
      // Gửi lệnh qua thư viện
      sendIRLibraryCommand(action);
      
    } else if (irMethod == "learned") {
      // Load learned commands nếu chưa có
      if (learnedPowerOnLen == 0 && learnedPowerOffLen == 0) {
        loadLearnedCommands();
      }

      // Gửi lệnh đã học
      sendIRLearnedCommand(action);
      
    } else {
      Serial.println("✗ Chua cau hinh IR");
    }
    
    // ===== QUAN TRỌNG: XÓA TOÀN BỘ NODE COMMANDS =====
    Serial.println();
    Serial.println("Dang xoa lenh...");
    
    if (Firebase.deleteNode(firebaseData, basePath)) {
      Serial.println("✓ Da xoa lenh thanh cong");
    } else {
      Serial.println("✗ Loi xoa lenh:");
      Serial.println(firebaseData.errorReason());
    }
    
    // Đánh dấu đã thực thi (tạo node mới với executed = true)
    String executedPath = basePath + "/lastExecuted";
    Firebase.setInt(firebaseData, executedPath, millis());
    
    Serial.println("═════════════════════════════════════");
    Serial.println();
  }
}

// ═══════════════════════════════════════
// LOAD LỆNH ĐÃ HỌC TỪ FIREBASE
// ═══════════════════════════════════════
void loadLearnedCommands() {
  Serial.println("Dang load lenh da hoc tu Firebase...");

  String basePath = "devices/" + deviceId + "/ir/learned/commands";

  // Load POWER_ON
  if (Firebase.getString(firebaseData, basePath + "/power_on")) {
    String rawData = firebaseData.stringData();
    if (rawData.length() > 0) {
      parseRawData(rawData, learnedPowerOn, learnedPowerOnLen);
      Serial.print("  ✓ POWER_ON loaded (");
      Serial.print(learnedPowerOnLen);
      Serial.println(" pulses)");
    }
  }

  // Load POWER_OFF
  if (Firebase.getString(firebaseData, basePath + "/power_off")) {
    String rawData = firebaseData.stringData();
    if (rawData.length() > 0) {
      parseRawData(rawData, learnedPowerOff, learnedPowerOffLen);
      Serial.print("  ✓ POWER_OFF loaded (");
      Serial.print(learnedPowerOffLen);
      Serial.println(" pulses)");
    }
  }

  // Load TEMP_UP
  if (Firebase.getString(firebaseData, basePath + "/temp_up")) {
    String rawData = firebaseData.stringData();
    if (rawData.length() > 0) {
      parseRawData(rawData, learnedTempUp, learnedTempUpLen);
      Serial.print("  ✓ TEMP_UP loaded (");
      Serial.print(learnedTempUpLen);
      Serial.println(" pulses)");
    }
  }

  // Load TEMP_DOWN
  if (Firebase.getString(firebaseData, basePath + "/temp_down")) {
    String rawData = firebaseData.stringData();
    if (rawData.length() > 0) {
      parseRawData(rawData, learnedTempDown, learnedTempDownLen);
      Serial.print("  ✓ TEMP_DOWN loaded (");
      Serial.print(learnedTempDownLen);
      Serial.println(" pulses)");
    }
  }

  Serial.println();
}

// ═══════════════════════════════════════
// PARSE RAW DATA STRING → ARRAY
// ═══════════════════════════════════════
void parseRawData(String data, uint16_t* buffer, uint16_t& len) {
  len = 0;
  int startIndex = 0;
  
  // Format: "1234,5678,9012,..."
  while (startIndex < data.length() && len < CAPTURE_BUFFER_SIZE) {
    int commaIndex = data.indexOf(',', startIndex);
    
    if (commaIndex == -1) {
      // Phần tử cuối
      buffer[len++] = data.substring(startIndex).toInt();
      break;
    } else {
      buffer[len++] = data.substring(startIndex, commaIndex).toInt();
      startIndex = commaIndex + 1;
    }
  }
}

// ═══════════════════════════════════════
// KIỂM TRA VÀ XỬ LÝ HỌC LỆNH IR
// ═══════════════════════════════════════
void checkIRLearning() {
  // Kiểm tra có session học lệnh active không
  String activePath = "ir_learning/" + deviceId + "/active";
  
  static bool lastActiveState = false;
  static unsigned long learningStartTime = 0;
  static int currentStep = 0;
  
  if (Firebase.getBool(firebaseData, activePath)) {
    bool isActive = firebaseData.boolData();
    
    // Bắt đầu session mới
    if (isActive && !lastActiveState) {
      Serial.println("═════════════════════════════════════");
      Serial.println("BAT DAU SESSION HOC LENH IR");
      Serial.println("═════════════════════════════════════");
      
      // Lấy step hiện tại
      String stepPath = "ir_learning/" + deviceId + "/currentStep";
      if (Firebase.getInt(firebaseData, stepPath)) {
        currentStep = firebaseData.intData();
      }
      
      Serial.print("Step: ");
      Serial.print(currentStep);
      Serial.print(" - ");

      if (currentStep == 0) {
        Serial.println("HOC LENH POWER_ON");
      } else if (currentStep == 1) {
        Serial.println("HOC LENH POWER_OFF");
      } else if (currentStep == 2) {
        Serial.println("HOC LENH TEMP_UP");
      } else if (currentStep == 3) {
        Serial.println("HOC LENH TEMP_DOWN");
      }
      
      Serial.println("Hay nhan nut tren remote trong 10 giay...");
      Serial.println("═════════════════════════════════════");
      Serial.println();
      
      learningStartTime = millis();
      
      // Xóa buffer cũ
      while (irrecv.decode(&irResults)) {
        irrecv.resume();
      }
      irrecv.resume();
      
      // Đánh dấu đang chờ
      String waitingPath = "ir_learning/" + deviceId + "/waiting";
      Firebase.setBool(firebaseData, waitingPath, true);
      
      lastActiveState = true;
    }
    
    // Đang trong session học
    if (isActive && lastActiveState) {
      // Timeout sau 10 giây
      if (millis() - learningStartTime > 10000) {
        Serial.println("TIMEOUT - Khong nhan duoc tin hieu");
        
        // Báo lỗi về Firebase
        String resultPath = "ir_learning/" + deviceId + "/result";
        Firebase.setBool(firebaseData, resultPath + "/success", false);
        Firebase.setString(firebaseData, resultPath + "/code", "");
        
        String waitingPath = "ir_learning/" + deviceId + "/waiting";
        Firebase.setBool(firebaseData, waitingPath, false);
        
        String activePath = "ir_learning/" + deviceId + "/active";
        Firebase.setBool(firebaseData, activePath, false);
        
        lastActiveState = false;
        return;
      }
      
      // Kiểm tra có tín hiệu mới không
      if (irrecv.decode(&irResults)) {
        // Kiểm tra độ dài hợp lệ
        if (irResults.rawlen >= 50) {
          Serial.println("✓ NHAN DUOC TIN HIEU HOP LE!");
          Serial.print("  Do dai: ");
          Serial.print(irResults.rawlen - 1);
          Serial.println(" pulses");
          Serial.print("  Protocol: ");
          Serial.println(typeToString(irResults.decode_type));
          
          // Lưu vào buffer tương ứng
          uint16_t rawLen = irResults.rawlen - 1;
          if (rawLen > CAPTURE_BUFFER_SIZE - 1) {
            rawLen = CAPTURE_BUFFER_SIZE - 1;
          }
          
          uint16_t* targetBuffer = nullptr;
          uint16_t* targetLen = nullptr;
          String commandName = "";

          if (currentStep == 0) {
            targetBuffer = learnedPowerOn;
            targetLen = &learnedPowerOnLen;
            commandName = "power_on";
          } else if (currentStep == 1) {
            targetBuffer = learnedPowerOff;
            targetLen = &learnedPowerOffLen;
            commandName = "power_off";
          } else if (currentStep == 2) {
            targetBuffer = learnedTempUp;
            targetLen = &learnedTempUpLen;
            commandName = "temp_up";
          } else if (currentStep == 3) {
            targetBuffer = learnedTempDown;
            targetLen = &learnedTempDownLen;
            commandName = "temp_down";
          }
          
          // Copy data
          *targetLen = rawLen;
          for (uint16_t i = 1; i <= rawLen; i++) {
            targetBuffer[i - 1] = irResults.rawbuf[i] * kRawTick;
          }
          
          // Tạo raw data string để lưu Firebase
          String rawDataString = "";
          for (uint16_t i = 0; i < rawLen; i++) {
            rawDataString += String(targetBuffer[i]);
            if (i < rawLen - 1) {
              rawDataString += ",";
            }
          }
          
          Serial.println("  Dang luu vao Firebase...");
          
          // Lưu vào Firebase
          String savePath = "devices/" + deviceId + "/ir/learned/commands/" + commandName;
          Firebase.setString(firebaseData, savePath, rawDataString);
          
          // Cập nhật kết quả
          String resultPath = "ir_learning/" + deviceId + "/result";
          Firebase.setBool(firebaseData, resultPath + "/success", true);
          Firebase.setString(firebaseData, resultPath + "/code", rawDataString);
          Firebase.setString(firebaseData, resultPath + "/protocol", typeToString(irResults.decode_type));
          Firebase.setInt(firebaseData, resultPath + "/bits", irResults.bits);
          
          // Tắt waiting
          String waitingPath = "ir_learning/" + deviceId + "/waiting";
          Firebase.setBool(firebaseData, waitingPath, false);
          
          Serial.println("  ✓ Da luu thanh cong!");
          Serial.println();
          
          // Kết thúc session
          lastActiveState = false;
          
        } else {
          Serial.println("✗ Tin hieu qua ngan (co the la nhieu)");
        }
        
        irrecv.resume();
      }
    }
    
  } else {
    lastActiveState = false;
  }
}

// ═══════════════════════════════════════
// PRINT BANNER
// ═══════════════════════════════════════
void printBanner() {
  Serial.println();
  Serial.println("═════════════════════════════════════");
  Serial.println("  AC CONTROL - ESP32 FIRMWARE v1.0");
  Serial.println("═════════════════════════════════════");
  Serial.println("Tinh nang:");
  Serial.println("  • Setup qua WiFi AP");
  Serial.println("  • Firebase Realtime Database");
  Serial.println("  • Dieu khien AC qua IR");
  Serial.println("  • Doc cam bien: DHT22, HLK, PZEM");
  Serial.println("  • Hoc lenh tu remote");
  Serial.println("  • Reset bang nut nhan (5s)");
  Serial.println("═════════════════════════════════════");
  Serial.println();
}