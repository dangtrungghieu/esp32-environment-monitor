# TÀI LIỆU KỸ THUẬT THUẬT TOÁN ESP32 FIRMWARE v1.0

> **Dự án**: AC Control - ESP32 Firmware
> **Version**: 1.0.0
> **Ngày**: 2025-12-07

---

## MỤC LỤC

1. [Thuật toán xử lý và thực thi lệnh điều khiển](#1-thuật-toán-xử-lý-và-thực-thi-lệnh-điều-khiển)
2. [Thuật toán phát hiện lỗi và tự phục hồi](#2-thuật-toán-phát-hiện-lỗi-và-tự-phục-hồi)
3. [Thuật toán học và tái tạo tín hiệu hồng ngoại](#3-thuật-toán-học-và-tái-tạo-tín-hiệu-hồng-ngoại)

---

# 1. THUẬT TOÁN XỬ LÝ VÀ THỰC THI LỆNH ĐIỀU KHIỂN

## 1.1. Tổng quan

Hệ thống sử dụng **Firebase Realtime Database Stream** để nhận lệnh real-time từ server/app, không cần polling.

### Luồng xử lý tổng quát:

```
┌─────────────┐
│ App/Server  │
└──────┬──────┘
       │ Set command
       ▼
┌─────────────────────┐
│ Firebase RTDB       │
│ commands/{deviceId} │
└──────┬──────────────┘
       │ Stream event
       ▼
┌─────────────────────┐
│ ESP32 Listener      │
│ checkCommands()     │
└──────┬──────────────┘
       │
       ├─► Parse command
       ├─► Determine IR method
       ├─► Execute IR command
       ├─► Update AC state
       └─► Delete command
```

---

## 1.2. Thuật toán Firebase Stream Listener

**File**: `main.cpp` - Hàm `checkCommands()` (dòng 1814-1935)

### Pseudocode:

```
FUNCTION checkCommands():
    // Bước 1: Kiểm tra stream có data mới
    IF NOT Firebase.readStream(streamData) THEN
        RETURN  // Không có data mới
    END IF

    IF NOT streamData.streamAvailable() THEN
        RETURN  // Stream không có thay đổi
    END IF

    // Bước 2: Parse command từ Firebase
    basePath = "commands/" + deviceId

    action = Firebase.getString(basePath + "/action")
    IF action == "" OR action == "null" THEN
        RETURN  // Lệnh rỗng
    END IF

    value = Firebase.getInt(basePath + "/value")  // Optional

    // Bước 3: Xử lý lệnh đặc biệt
    IF action == "reset" THEN
        Firebase.deleteNode(basePath)
        factoryReset()
        RETURN
    END IF

    IF action == "setTemp" THEN
        IF value >= 16 AND value <= 30 THEN
            currentTemp = value
        END IF
    END IF

    // Bước 4: Xác định phương thức IR
    irMethod = Firebase.getString("devices/" + deviceId + "/ir/method")

    IF irMethod == "library" THEN
        // Bước 4a: Sử dụng thư viện AC có sẵn
        IF currentProtocol == "" THEN
            brandId = Firebase.getString("devices/" + deviceId + "/ir/library/brandId")
            protocol = Firebase.getString("ir_library/" + brandId + "/protocol")
            initializeIRProtocol(protocol)
        END IF
        sendIRLibraryCommand(action)

    ELSE IF irMethod == "learned" THEN
        // Bước 4b: Sử dụng lệnh đã học
        IF learnedCommands are empty THEN
            loadLearnedCommands()
        END IF
        sendIRLearnedCommand(action)

    ELSE
        LOG("Chưa cấu hình IR")
    END IF

    // Bước 5: Xóa lệnh đã thực thi
    Firebase.deleteNode(basePath)
    Firebase.setInt(basePath + "/lastExecuted", millis())

END FUNCTION
```

### Timing Diagram:

```
Time ─────────────────────────────────────────────────►

App:     │ Write cmd │             │             │
         │           │             │             │
Firebase:│           │ Notify      │             │
         │           │             │             │
ESP32:   │           │ Receive     │ Execute IR  │ Delete cmd
         │           │ ◄50-200ms►  │ ◄200ms►     │ ◄100ms►
         │           │             │             │
         └───────────┴─────────────┴─────────────┴──────►
                     Total: ~350-500ms từ write đến done
```

---

## 1.3. Thuật toán IR Library Mode

**File**: `main.cpp` - Hàm `sendIRLibraryCommand()` (dòng 1574-1741)

### Cấu trúc:

Sử dụng các thư viện AC có sẵn từ **IRremoteESP8266** với state-based control.

### Pseudocode chi tiết:

```
FUNCTION sendIRLibraryCommand(action):
    LOG("Gửi lệnh IR (Library): " + action)

    // Lấy AC object tương ứng với protocol hiện tại
    acObject = getACObject(currentProtocol)  // daikinAC, panasonicAC, etc.

    IF acObject == NULL THEN
        LOG("Protocol chưa khởi tạo")
        RETURN
    END IF

    // Xử lý từng loại lệnh
    SWITCH action:
        CASE "power":
            IF acPowerOn == TRUE THEN
                acObject.off()
                acPowerOn = FALSE
            ELSE
                acObject.on()
                acObject.setTemp(currentTemp)
                acObject.setMode(COOL)
                acObject.setFan(AUTO)
                acPowerOn = TRUE
            END IF

        CASE "tempUp":
            IF acPowerOn == TRUE THEN
                IF currentTemp < 30 THEN
                    currentTemp = currentTemp + 1
                    acObject.setTemp(currentTemp)
                END IF
            END IF

        CASE "tempDown":
            IF acPowerOn == TRUE THEN
                IF currentTemp > 16 THEN
                    currentTemp = currentTemp - 1
                    acObject.setTemp(currentTemp)
                END IF
            END IF

        CASE "setTemp":
            IF acPowerOn == TRUE THEN
                acObject.setTemp(currentTemp)
            END IF
    END SWITCH

    // Gửi tín hiệu IR
    acObject.send()

    LOG("Đã gửi tín hiệu IR")

END FUNCTION
```

### State Machine cho Power Control:

```
       ┌─────────┐  power  ┌─────────┐
       │   OFF   ├────────►│   ON    │
       │         │◄────────┤         │
       └─────────┘  power  └─────────┘
           │                    │
           │                    ├─► setTemp(currentTemp)
           │                    ├─► setMode(COOL)
           │                    ├─► setFan(AUTO)
           │                    └─► tempUp/tempDown (±1°C)
           │
           └─► tempUp/tempDown: IGNORED
```

---

## 1.4. Thuật toán IR Learned Mode

**File**: `main.cpp` - Hàm `sendIRLearnedCommand()` (dòng 1746-1809)

### Đặc điểm:

- Sử dụng **4 lệnh RAW riêng biệt**: `POWER_ON`, `POWER_OFF`, `TEMP_UP`, `TEMP_DOWN`
- Smart power logic: Tự động chọn ON/OFF dựa trên trạng thái hiện tại
- Raw timing data (uint16_t array)

### Pseudocode:

```
FUNCTION sendIRLearnedCommand(action):
    LOG("Gửi lệnh IR (Learned): " + action)

    // Bước 1: Tắt IR receiver (tránh nhiễu)
    irrecv.disableIRIn()
    DELAY(50)

    // Bước 2: Xác định lệnh cần gửi
    SWITCH action:
        CASE "power":
            // Smart power logic
            IF acPowerOn == TRUE THEN
                // AC đang BẬT → Gửi lệnh TẮT
                IF learnedPowerOffLen > 0 THEN
                    LOG("Phát POWER_OFF (" + learnedPowerOffLen + " pulses)")
                    irsend.sendRaw(learnedPowerOff, learnedPowerOffLen, 38)
                    DELAY(200)
                    acPowerOn = FALSE
                ELSE
                    LOG("Chưa học lệnh POWER_OFF")
                END IF
            ELSE
                // AC đang TẮT → Gửi lệnh BẬT
                IF learnedPowerOnLen > 0 THEN
                    LOG("Phát POWER_ON (" + learnedPowerOnLen + " pulses)")
                    irsend.sendRaw(learnedPowerOn, learnedPowerOnLen, 38)
                    DELAY(200)
                    acPowerOn = TRUE
                ELSE
                    LOG("Chưa học lệnh POWER_ON")
                END IF
            END IF

        CASE "tempUp":
            IF learnedTempUpLen > 0 THEN
                LOG("Phát TEMP_UP (" + learnedTempUpLen + " pulses)")
                irsend.sendRaw(learnedTempUp, learnedTempUpLen, 38)
                DELAY(200)
                IF currentTemp < 30 THEN
                    currentTemp = currentTemp + 1
                END IF
            ELSE
                LOG("Chưa học lệnh TEMP_UP")
            END IF

        CASE "tempDown":
            IF learnedTempDownLen > 0 THEN
                LOG("Phát TEMP_DOWN (" + learnedTempDownLen + " pulses)")
                irsend.sendRaw(learnedTempDown, learnedTempDownLen, 38)
                DELAY(200)
                IF currentTemp > 16 THEN
                    currentTemp = currentTemp - 1
                END IF
            ELSE
                LOG("Chưa học lệnh TEMP_DOWN")
            END IF

        DEFAULT:
            LOG("Chưa học lệnh này")
    END SWITCH

    LOG("Đã gửi tín hiệu IR")
    DELAY(100)

    // Bước 3: Bật lại IR receiver
    irrecv.enableIRIn()

END FUNCTION
```

### Smart Power Decision Tree:

```
                    ┌─────────────┐
                    │ action=power│
                    └──────┬──────┘
                           │
              ┌────────────┴────────────┐
              │                         │
         ┌────▼────┐              ┌────▼────┐
         │acPowerOn│              │acPowerOn│
         │= TRUE   │              │= FALSE  │
         └────┬────┘              └────┬────┘
              │                         │
      ┌───────▼────────┐        ┌───────▼────────┐
      │ Send POWER_OFF │        │ Send POWER_ON  │
      │ acPowerOn=FALSE│        │ acPowerOn=TRUE │
      └────────────────┘        └────────────────┘
```

---

## 1.5. Thuật toán Auto-Detect AC Power State

**File**: `main.cpp` - Hàm `readSensors()` (dòng 1391-1407)

### Mục đích:

Tự động phát hiện trạng thái AC (bật/tắt) dựa trên dòng điện PZEM, đồng bộ với trạng thái thực tế.

### Pseudocode:

```
FUNCTION autoDetectACPower():
    // Ngưỡng phát hiện: 0.2A
    THRESHOLD = 0.2

    // Đọc dòng điện từ PZEM
    detectedPowerOn = (powerCurrent > THRESHOLD)

    // So sánh với trạng thái hiện tại
    IF detectedPowerOn != acPowerOn THEN
        // Phát hiện thay đổi
        acPowerOn = detectedPowerOn

        LOG("==========================================")
        LOG("TỰ ĐỘNG CẬP NHẬT TRẠNG THÁI AC!")
        LOG("Dòng điện: " + powerCurrent + " A")
        LOG("Trạng thái mới: " + (acPowerOn ? "BẬT" : "TẮT"))
        LOG("==========================================")

        // Không cần update Firebase ở đây
        // Sẽ tự động update trong updateStatus() (mỗi 5s)
    END IF

END FUNCTION
```

### Use Case:

```
Scenario 1: User bật AC bằng remote vật lý
─────────────────────────────────────────────
t=0s:   acPowerOn = FALSE, powerCurrent = 0.0A
t=1s:   User nhấn remote → AC bật
t=3s:   PZEM đọc: powerCurrent = 3.5A
        → detectedPowerOn = TRUE
        → acPowerOn = TRUE (auto update)
t=8s:   Firebase status update: powerOn = true

Scenario 2: User tắt AC bằng remote vật lý
─────────────────────────────────────────────
t=0s:   acPowerOn = TRUE, powerCurrent = 3.2A
t=1s:   User nhấn remote → AC tắt
t=3s:   PZEM đọc: powerCurrent = 0.1A
        → detectedPowerOn = FALSE
        → acPowerOn = FALSE (auto update)
t=8s:   Firebase status update: powerOn = false
```

### Lợi ích:

1. **Đồng bộ state** khi user dùng remote vật lý
2. **Không cần feedback** từ AC (one-way IR)
3. **Tự động sửa lỗi** nếu lệnh IR không thành công

---

# 2. THUẬT TOÁN PHÁT HIỆN LỖI VÀ TỰ PHỤC HỒI

## 2.1. Tổng quan Error Recovery

Hệ thống có **3 cơ chế tự phục hồi** chính:

| Sensor | Phương pháp phát hiện | Ngưỡng lỗi | Cơ chế phục hồi |
|--------|----------------------|-------------|-----------------|
| DHT22 | Giá trị NaN | 2 lần liên tiếp | Pin reset |
| PZEM-004T | NaN hoặc Stuck values | 3 lần lỗi / 5 lần stuck | Serial reset |
| WiFi/Firebase | Connection timeout | 5 retries | ESP restart |

---

## 2.2. Thuật toán DHT22 Error Recovery

**File**: `main.cpp` - Hàm `readSensors()` (dòng 1283-1311) và `resetDHT22()` (dòng 1202-1219)

### Chiến lược:

- **Phát hiện**: `isnan(temp)` hoặc `isnan(humidity)`
- **Counter-based**: Đếm số lỗi liên tiếp
- **Recovery**: Power cycle pin (OUTPUT LOW → INPUT_PULLUP)

### Pseudocode:

```
// Biến global
dhtErrorCount = 0
DHT_MAX_ERRORS = 2

FUNCTION readDHT22():
    // Bước 1: Đọc sensor
    temp = dht.readTemperature()
    humidity = dht.readHumidity()

    // Bước 2: Kiểm tra lỗi
    IF isnan(temp) OR isnan(humidity) THEN
        LOG("Lỗi đọc DHT22")
        dhtErrorCount = dhtErrorCount + 1

        // Bước 3: Kiểm tra ngưỡng lỗi
        IF dhtErrorCount >= DHT_MAX_ERRORS THEN
            // Thực hiện reset
            resetDHT22()

            // Đọc lại sau reset
            temp = dht.readTemperature()
            humidity = dht.readHumidity()

            IF NOT isnan(temp) AND NOT isnan(humidity) THEN
                LOG("DHT22 hoạt động lại sau reset")
                roomTemp = temp
                roomHumidity = humidity
            ELSE
                // Vẫn lỗi → Dùng giá trị 0
                roomTemp = 0
                roomHumidity = 0
            END IF
        ELSE
            // Chưa đủ số lần lỗi
            roomTemp = 0
            roomHumidity = 0
        END IF

    ELSE
        // Đọc thành công → Reset counter
        dhtErrorCount = 0
        roomTemp = temp
        roomHumidity = humidity
    END IF

END FUNCTION

FUNCTION resetDHT22():
    LOG("DHT22 bị treo - Đang reset...")

    // Bước 1: Tắt nguồn DHT bằng OUTPUT LOW
    pinMode(DHT_PIN, OUTPUT)
    digitalWrite(DHT_PIN, LOW)
    DELAY(500)  // Chờ discharge hoàn toàn

    // Bước 2: Bật lại DHT
    pinMode(DHT_PIN, INPUT_PULLUP)
    DELAY(100)

    // Bước 3: Khởi tạo lại thư viện
    dht.begin()
    DELAY(2000)  // DHT22 cần 1-2s để ổn định

    LOG("Đã reset DHT22 thành công")
    dhtErrorCount = 0

END FUNCTION
```

### State Diagram:

```
┌──────────────┐
│   NORMAL     │
│  errorCount=0│
└───────┬──────┘
        │
        │ Read error
        ▼
┌──────────────┐
│  ERROR_1     │
│ errorCount=1 │
└───────┬──────┘
        │
        ├─► Success → NORMAL (reset counter)
        │
        │ Read error
        ▼
┌──────────────┐
│  ERROR_2     │
│ errorCount=2 │
│  THRESHOLD!  │
└───────┬──────┘
        │
        │ Trigger reset
        ▼
┌──────────────┐
│ RESET_DHT22  │
│ - Pin cycle  │
│ - dht.begin()│
│ - Delay 2s   │
└───────┬──────┘
        │
        ├─► Success → NORMAL
        └─► Fail → Use value=0
```

---

## 2.3. Thuật toán PZEM-004T Error Recovery

**File**: `main.cpp` - Hàm `readSensors()` (dòng 1316-1446) và `resetPZEM()` (dòng 1225-1271)

### Đặc điểm:

PZEM có **2 loại lỗi**:

1. **Read Error**: Trả về NaN hoặc 0
2. **Stuck Error**: Giá trị không thay đổi (sensor bị treo)

### Pseudocode chi tiết:

```
// Biến global
pzemErrorCount = 0
pzemStuckCount = 0
PZEM_MAX_ERRORS = 3
PZEM_STUCK_THRESHOLD = 5
lastValidVoltage = 0
lastValidCurrent = 0
lastValidPower = 0

FUNCTION readPZEM():
    // Bước 1: Đọc sensor
    voltage = pzem.voltage()

    IF NOT isnan(voltage) AND voltage > 0 THEN
        // ════════════════════════════════════
        // TRƯỜNG HỢP 1: ĐỌC THÀNH CÔNG
        // ════════════════════════════════════
        current = pzem.current()
        power = pzem.power()

        // Bước 2a: KIỂM TRA STUCK (chỉ khi đã có giá trị cũ và power > 100W)
        IF lastValidVoltage > 0 AND power > 100 THEN
            voltageDiff = ABS(voltage - lastValidVoltage)
            currentDiff = ABS(current - lastValidCurrent)
            powerDiff = ABS(power - lastValidPower)

            LOG("[STUCK CHECK] V_diff=" + voltageDiff +
                " A_diff=" + currentDiff +
                " W_diff=" + powerDiff)

            // Ngưỡng stuck: V<0.1, A<0.01, W<1.0
            IF voltageDiff < 0.1 AND currentDiff < 0.01 AND powerDiff < 1.0 THEN
                pzemStuckCount = pzemStuckCount + 1
                LOG("⚠️ GIÁ TRỊ STUCK (" + pzemStuckCount + "/" + PZEM_STUCK_THRESHOLD + ")")

                // Đạt ngưỡng stuck
                IF pzemStuckCount >= PZEM_STUCK_THRESHOLD THEN
                    LOG("⚠️⚠️⚠️ PHÁT HIỆN PZEM BỊ STUCK!")
                    LOG("Giá trị đứng im: V=" + voltage + " A=" + current + " W=" + power)

                    // Force reset
                    resetPZEM()
                    pzemStuckCount = 0

                    // Đọc lại
                    voltage = pzem.voltage()
                    current = pzem.current()
                    power = pzem.power()
                END IF
            ELSE
                // Giá trị có thay đổi → reset stuck counter
                IF pzemStuckCount > 0 THEN
                    LOG("✓ Giá trị thay đổi - reset stuck counter")
                END IF
                pzemStuckCount = 0
            END IF
        END IF

        // Bước 2b: Lưu giá trị hợp lệ
        lastValidVoltage = voltage
        IF NOT isnan(current) THEN
            lastValidCurrent = current
        END IF
        IF NOT isnan(power) THEN
            lastValidPower = power
        END IF

        // Bước 2c: Reset error counter
        pzemErrorCount = 0

        // Bước 2d: Auto-detect AC power
        detectedPowerOn = (current > 0.2)
        IF detectedPowerOn != acPowerOn THEN
            acPowerOn = detectedPowerOn
            LOG("TỰ ĐỘNG CẬP NHẬT TRẠNG THÁI AC: " + (acPowerOn ? "BẬT" : "TẮT"))
        END IF

        // Cập nhật giá trị
        powerCurrent = current
        powerWatt = power

    ELSE
        // ════════════════════════════════════
        // TRƯỜNG HỢP 2: ĐỌC THẤT BẠI
        // ════════════════════════════════════
        LOG("✗ Lỗi đọc PZEM-004T")
        pzemErrorCount = pzemErrorCount + 1

        IF pzemErrorCount >= PZEM_MAX_ERRORS THEN
            // Reset PZEM
            resetPZEM()

            // Đọc lại
            voltage = pzem.voltage()
            IF NOT isnan(voltage) AND voltage > 0 THEN
                current = pzem.current()
                power = pzem.power()

                lastValidVoltage = voltage
                IF NOT isnan(current) THEN lastValidCurrent = current END IF
                IF NOT isnan(power) THEN lastValidPower = power END IF

                LOG("✓ PZEM hoạt động lại sau reset")
            ELSE
                // Vẫn lỗi → Dùng giá trị cũ
                LOG("⚠️ PZEM vẫn lỗi - Sử dụng giá trị cũ")
                powerCurrent = lastValidCurrent
                powerWatt = lastValidPower
            END IF
        ELSE
            // Chưa đủ số lần lỗi → Dùng giá trị cũ
            powerCurrent = lastValidCurrent
            powerWatt = lastValidPower
        END IF

        // Không đọc được → coi như AC tắt
        IF acPowerOn AND pzemErrorCount >= PZEM_MAX_ERRORS THEN
            acPowerOn = FALSE
            LOG("⚠️ Mất tín hiệu PZEM - Đặt AC = TẮT")
        END IF
    END IF

END FUNCTION

FUNCTION resetPZEM():
    LOG("⚠️ PZEM-004T bị treo - Đang reset...")
    LOG("THÔNG TIN TRƯỚC KHI RESET:")
    LOG("  Last valid voltage: " + lastValidVoltage + " V")
    LOG("  Last valid current: " + lastValidCurrent + " A")
    LOG("  Last valid power: " + lastValidPower + " W")

    // Bước 1: Dừng serial cũ
    pzemSerial.end()
    DELAY(500)

    // Bước 2: Khởi tạo lại serial và PZEM
    pzemSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN)
    DELAY(1000)

    LOG("✓ Đã reset PZEM-004T thành công")
    LOG("Chờ PZEM ổn định...")
    DELAY(2000)

    // Bước 3: Test đọc lại
    testVoltage = pzem.voltage()
    IF NOT isnan(testVoltage) THEN
        LOG("✓ PZEM hoạt động lại - Voltage: " + testVoltage + " V")

        // Reset tất cả counters
        pzemErrorCount = 0
        pzemStuckCount = 0
    ELSE
        LOG("✗ PZEM vẫn chưa hoạt động")
    END IF

END FUNCTION
```

### Stuck Detection Flow:

```
Read cycle n:     V=220.0  A=3.50  W=770.0
                        │
                        ▼ Compare with last
Read cycle n+1:   V=220.0  A=3.50  W=770.0
                  diff: 0.0   0.0    0.0    → stuckCount++
                        │
Read cycle n+2:   V=220.0  A=3.50  W=770.0  → stuckCount++
Read cycle n+3:   V=220.0  A=3.50  W=770.0  → stuckCount++
Read cycle n+4:   V=220.0  A=3.50  W=770.0  → stuckCount++
Read cycle n+5:   V=220.0  A=3.50  W=770.0  → stuckCount++ = 5
                        │
                        ▼ THRESHOLD REACHED!
                   ┌────────────┐
                   │ resetPZEM()│
                   └─────┬──────┘
                         │
Read cycle n+6:   V=220.2  A=3.48  W=768.5  → NORMAL (values changed)
                  stuckCount = 0
```

---

## 2.4. Thuật toán WiFi/Firebase Recovery

**File**: `main.cpp` - Hàm `connectWiFi()` (dòng 800-829) và `connectFirebase()` (dòng 883-961)

### WiFi Recovery:

```
FUNCTION connectWiFi():
    LOG("Đang kết nối WiFi...")
    LOG("SSID: " + wifiSSID)

    WiFi.begin(wifiSSID, wifiPassword)

    attempts = 0
    WHILE WiFi.status() != WL_CONNECTED AND attempts < 30 DO
        DELAY(500)
        LOG(".")
        attempts = attempts + 1
    END WHILE

    IF WiFi.status() == WL_CONNECTED THEN
        LOG("✓ WiFi đã kết nối")
        LOG("IP: " + WiFi.localIP())
    ELSE
        LOG("✗ Không thể kết nối WiFi")
        LOG("Vui lòng cấu hình lại")

        // Reset và vào setup mode
        factoryReset()
    END IF

END FUNCTION
```

### Firebase Recovery:

```
FUNCTION connectFirebase():
    LOG("Đang kết nối Firebase...")

    // Cấu hình
    firebaseConfig.host = FIREBASE_HOST
    firebaseConfig.signer.tokens.legacy_token = FIREBASE_AUTH

    // Timeout settings
    firebaseConfig.timeout.serverResponse = 15000    // 15s
    firebaseConfig.timeout.socketConnection = 15000  // 15s
    firebaseConfig.timeout.sslHandshake = 60000      // 60s
    firebaseConfig.timeout.rtdbKeepAlive = 45000     // 45s

    // Disable WiFi sleep
    WiFi.setSleep(false)

    Firebase.begin(&firebaseConfig, &firebaseAuth)
    Firebase.reconnectWiFi(true)

    DELAY(2000)

    // Kiểm tra kết nối bằng test write
    retryCount = 0
    connected = false

    WHILE NOT connected AND retryCount < 5 DO
        testPath = "devices/" + deviceId + "/info/macAddress"
        macAddress = WiFi.macAddress()

        IF Firebase.setString(firebaseData, testPath, macAddress) THEN
            LOG("✓ Firebase đã kết nối thành công!")
            connected = true
        ELSE
            LOG("✗ Lần thử " + (retryCount + 1) + " thất bại: " + firebaseData.errorReason())
            retryCount = retryCount + 1
            DELAY(2000)
        END IF
    END WHILE

    IF NOT connected THEN
        LOG("✗✗✗ KHÔNG THỂ KẾT NỐI FIREBASE!")
        LOG("Thiết bị sẽ khởi động lại sau 10 giây...")
        DELAY(10000)
        ESP.restart()
        RETURN
    END IF

    // Setup stream
    streamPath = "commands/" + deviceId
    IF NOT Firebase.beginStream(streamData, streamPath) THEN
        LOG("✗ Lỗi khởi tạo Firebase Stream")
        LOG(streamData.errorReason())
    ELSE
        LOG("✓ Firebase Stream đã sẵn sàng")
    END IF

END FUNCTION
```

### Retry Strategy:

```
WiFi Connection:
  Timeout: 30 × 500ms = 15 seconds
  On fail: factoryReset() → Setup mode

Firebase Connection:
  Retry: 5 times × 2s delay = 10 seconds
  On fail: ESP.restart()

NTP Sync:
  Retry: 30 times × 1s = 30 seconds
  On fail: ESP.restart()
  Reason: SSL requires valid time
```

---

# 3. THUẬT TOÁN HỌC VÀ TÁI TẠO TÍN HIỆU HỒNG NGOẠI

## 3.1. Tổng quan IR Learning System

### Kiến trúc:

```
┌─────────────┐
│  App/Web    │
└──────┬──────┘
       │ Start learning session
       ▼
┌─────────────────────────────┐
│ Firebase                     │
│ ir_learning/{deviceId}/      │
│   - active: true             │
│   - currentStep: 0-3         │
│   - waiting: true            │
└──────┬──────────────────────┘
       │ Stream event
       ▼
┌─────────────────────────────┐
│ ESP32 IR Receiver           │
│ - Listen 10 seconds         │
│ - Decode IR signal          │
│ - Extract raw timing        │
└──────┬──────────────────────┘
       │ Save
       ▼
┌─────────────────────────────┐
│ Firebase                     │
│ devices/{deviceId}/ir/       │
│   learned/commands/          │
│     - power_on: "raw_data"   │
│     - power_off: "raw_data"  │
│     - temp_up: "raw_data"    │
│     - temp_down: "raw_data"  │
└─────────────────────────────┘
```

### 4 lệnh riêng biệt:

| Step | Command Name | Mục đích |
|------|-------------|----------|
| 0 | POWER_ON | Bật AC từ trạng thái tắt |
| 1 | POWER_OFF | Tắt AC từ trạng thái bật |
| 2 | TEMP_UP | Tăng nhiệt độ +1°C |
| 3 | TEMP_DOWN | Giảm nhiệt độ -1°C |

---

## 3.2. Thuật toán IR Learning Session

**File**: `main.cpp` - Hàm `checkIRLearning()` (dòng 2017-2182)

### State Machine:

```
┌──────────────┐
│    IDLE      │
│ active=false │
└───────┬──────┘
        │ App triggers learning
        ▼
┌──────────────────────┐
│  SESSION_START       │
│  - Clear IR buffer   │
│  - Set waiting=true  │
│  - Start 10s timer   │
└───────┬──────────────┘
        │
        ├───► WAITING_FOR_SIGNAL (max 10s)
        │
        ├─► Timeout → FAILED (waiting=false, active=false)
        │
        └─► Signal received
            ▼
        ┌──────────────────┐
        │ SIGNAL_RECEIVED  │
        │ - Validate       │
        │ - Extract raw    │
        │ - Save Firebase  │
        └───────┬──────────┘
                │
                ▼
        ┌──────────────┐
        │   SUCCESS    │
        │ waiting=false│
        │ active=false │
        └──────────────┘
```

### Pseudocode chi tiết:

```
// Biến static (giữ state giữa các lần gọi)
STATIC lastActiveState = false
STATIC learningStartTime = 0
STATIC currentStep = 0

FUNCTION checkIRLearning():
    // Bước 1: Kiểm tra session có active không
    activePath = "ir_learning/" + deviceId + "/active"
    isActive = Firebase.getBool(activePath)

    // ═══════════════════════════════════════
    // TRẠNG THÁI 1: BẮT ĐẦU SESSION MỚI
    // ═══════════════════════════════════════
    IF isActive == TRUE AND lastActiveState == FALSE THEN
        LOG("═════════════════════════════════════")
        LOG("BẮT ĐẦU SESSION HỌC LỆNH IR")
        LOG("═════════════════════════════════════")

        // Lấy step hiện tại (0-3)
        stepPath = "ir_learning/" + deviceId + "/currentStep"
        currentStep = Firebase.getInt(stepPath)

        LOG("Step: " + currentStep + " - " + getStepName(currentStep))
        LOG("Hãy nhấn nút trên remote trong 10 giây...")
        LOG("═════════════════════════════════════")

        learningStartTime = millis()

        // Xóa IR buffer cũ
        WHILE irrecv.decode(&irResults) DO
            irrecv.resume()
        END WHILE
        irrecv.resume()

        // Đánh dấu đang chờ
        Firebase.setBool("ir_learning/" + deviceId + "/waiting", true)

        lastActiveState = true
    END IF

    // ═══════════════════════════════════════
    // TRẠNG THÁI 2: ĐANG TRONG SESSION
    // ═══════════════════════════════════════
    IF isActive == TRUE AND lastActiveState == TRUE THEN
        // Kiểm tra timeout (10 giây)
        IF millis() - learningStartTime > 10000 THEN
            LOG("TIMEOUT - Không nhận được tín hiệu")

            // Báo lỗi về Firebase
            resultPath = "ir_learning/" + deviceId + "/result"
            Firebase.setBool(resultPath + "/success", false)
            Firebase.setString(resultPath + "/code", "")
            Firebase.setBool("ir_learning/" + deviceId + "/waiting", false)
            Firebase.setBool("ir_learning/" + deviceId + "/active", false)

            lastActiveState = false
            RETURN
        END IF

        // Kiểm tra có tín hiệu mới không
        IF irrecv.decode(&irResults) THEN
            // Bước 2a: Validate độ dài
            IF irResults.rawlen >= 50 THEN
                LOG("✓ NHẬN ĐƯỢC TÍN HIỆU HỢP LỆ!")
                LOG("  Độ dài: " + (irResults.rawlen - 1) + " pulses")
                LOG("  Protocol: " + typeToString(irResults.decode_type))

                // Bước 2b: Xác định target buffer
                rawLen = irResults.rawlen - 1
                IF rawLen > CAPTURE_BUFFER_SIZE - 1 THEN
                    rawLen = CAPTURE_BUFFER_SIZE - 1
                END IF

                targetBuffer = NULL
                targetLen = NULL
                commandName = ""

                SWITCH currentStep:
                    CASE 0:
                        targetBuffer = learnedPowerOn
                        targetLen = &learnedPowerOnLen
                        commandName = "power_on"
                    CASE 1:
                        targetBuffer = learnedPowerOff
                        targetLen = &learnedPowerOffLen
                        commandName = "power_off"
                    CASE 2:
                        targetBuffer = learnedTempUp
                        targetLen = &learnedTempUpLen
                        commandName = "temp_up"
                    CASE 3:
                        targetBuffer = learnedTempDown
                        targetLen = &learnedTempDownLen
                        commandName = "temp_down"
                END SWITCH

                // Bước 2c: Copy raw data
                *targetLen = rawLen
                FOR i = 1 TO rawLen DO
                    targetBuffer[i - 1] = irResults.rawbuf[i] * kRawTick
                END FOR

                // Bước 2d: Tạo CSV string
                rawDataString = ""
                FOR i = 0 TO rawLen - 1 DO
                    rawDataString = rawDataString + String(targetBuffer[i])
                    IF i < rawLen - 1 THEN
                        rawDataString = rawDataString + ","
                    END IF
                END FOR

                LOG("  Đang lưu vào Firebase...")

                // Bước 2e: Lưu vào Firebase
                savePath = "devices/" + deviceId + "/ir/learned/commands/" + commandName
                Firebase.setString(savePath, rawDataString)

                // Bước 2f: Cập nhật kết quả
                resultPath = "ir_learning/" + deviceId + "/result"
                Firebase.setBool(resultPath + "/success", true)
                Firebase.setString(resultPath + "/code", rawDataString)
                Firebase.setString(resultPath + "/protocol", typeToString(irResults.decode_type))
                Firebase.setInt(resultPath + "/bits", irResults.bits)

                // Bước 2g: Tắt waiting
                Firebase.setBool("ir_learning/" + deviceId + "/waiting", false)

                LOG("  ✓ Đã lưu thành công!")

                // Kết thúc session
                lastActiveState = false

            ELSE
                LOG("✗ Tín hiệu quá ngắn (có thể là nhiễu)")
            END IF

            irrecv.resume()
        END IF
    END IF

    // ═══════════════════════════════════════
    // TRẠNG THÁI 3: SESSION KẾT THÚC
    // ═══════════════════════════════════════
    IF isActive == FALSE THEN
        lastActiveState = false
    END IF

END FUNCTION

FUNCTION getStepName(step):
    SWITCH step:
        CASE 0: RETURN "HỌC LỆNH POWER_ON"
        CASE 1: RETURN "HỌC LỆNH POWER_OFF"
        CASE 2: RETURN "HỌC LỆNH TEMP_UP"
        CASE 3: RETURN "HỌC LỆNH TEMP_DOWN"
        DEFAULT: RETURN "UNKNOWN"
    END SWITCH
END FUNCTION
```

### Timing Diagram:

```
App:        │ Set active=true │                    │
            │ Set step=0      │                    │
            └─────────────────┘                    │
                     │                              │
Firebase:            │ Notify                       │
                     │                              │
ESP32:               ▼                              │
            ┌────────────────┐                      │
            │ Start session  │                      │
            │ Clear buffer   │                      │
            │ Set waiting    │                      │
            └────────┬───────┘                      │
                     │                              │
                     │◄──── 10 seconds timeout ────►│
                     │                              │
User:                │  Press remote button         │
                     │        │                     │
IR Signal:           │        ▼                     │
                     │   ┌─────────┐                │
                     │   │ Decode  │                │
                     │   └────┬────┘                │
                     │        │                     │
ESP32:               │        ▼                     │
                     │   ┌──────────────┐           │
                     │   │ Extract raw  │           │
                     │   │ Save Firebase│           │
                     │   └──────┬───────┘           │
                     │          │                   │
Firebase:            │          ▼                   │
                     │   ┌──────────────┐           │
                     │   │ result/      │           │
                     │   │  success=true│           │
                     │   │  code="..."  │           │
                     │   └──────────────┘           │
                     │                              │
            ◄────────┴──────────────────────────────┘
            Total: ~200-500ms từ IR signal đến Firebase save
```

---

## 3.3. Thuật toán Decode IR Signal

**IRremoteESP8266 library internal**

### Quá trình decode:

```
Raw IR Signal (từ receiver)
    │
    ▼
┌───────────────────────┐
│ Sample timing         │
│ - Mark (ON) time      │
│ - Space (OFF) time    │
└───────┬───────────────┘
        │
        ▼
┌───────────────────────┐
│ Convert to uint16_t[] │
│ rawbuf[0..rawlen]     │
│ Unit: kRawTick (50µs) │
└───────┬───────────────┘
        │
        ▼
┌───────────────────────┐
│ Protocol Detection    │
│ - NEC, Sony, RC5...   │
│ - Daikin, Panasonic.. │
│ - RAW (unknown)       │
└───────┬───────────────┘
        │
        ▼
┌───────────────────────┐
│ Extract data          │
│ - Protocol type       │
│ - Bits count          │
│ - Value (if decoded)  │
│ - Raw timing array    │
└───────────────────────┘
```

### Raw Data Format:

```
irResults.rawbuf[0] = unused
irResults.rawbuf[1] = first mark time (µs / 50)
irResults.rawbuf[2] = first space time (µs / 50)
irResults.rawbuf[3] = second mark time
...
irResults.rawlen = total elements (including [0])

Actual data: rawbuf[1] to rawbuf[rawlen-1]
Total pulses: rawlen - 1

Conversion to microseconds:
  realTime_µs = rawbuf[i] * kRawTick
  kRawTick = 50 (default)
```

### Ví dụ thực tế:

```
Remote button: POWER_ON của Daikin

Raw signal nhận được:
  rawlen = 425
  decode_type = DAIKIN
  bits = 312

Raw timing (first 10 elements):
  rawbuf[1] = 68   →  68 × 50 = 3400µs (header mark)
  rawbuf[2] = 34   →  34 × 50 = 1700µs (header space)
  rawbuf[3] = 8    →   8 × 50 =  400µs (mark)
  rawbuf[4] = 16   →  16 × 50 =  800µs (space)
  rawbuf[5] = 8    →   8 × 50 =  400µs (mark)
  ...

CSV format lưu Firebase:
  "3400,1700,400,800,400,400,400,1200,..."

Buffer size: 424 pulses × 2 bytes = 848 bytes
CSV string: ~5000 characters
```

---

## 3.4. Thuật toán Load Learned Commands

**File**: `main.cpp` - Hàm `loadLearnedCommands()` (dòng 1940-1990)

### Pseudocode:

```
FUNCTION loadLearnedCommands():
    LOG("Đang load lệnh đã học từ Firebase...")

    basePath = "devices/" + deviceId + "/ir/learned/commands"

    // Load POWER_ON
    IF Firebase.getString(basePath + "/power_on") THEN
        rawData = firebaseData.stringData()
        IF rawData.length() > 0 THEN
            parseRawData(rawData, learnedPowerOn, learnedPowerOnLen)
            LOG("  ✓ POWER_ON loaded (" + learnedPowerOnLen + " pulses)")
        END IF
    END IF

    // Load POWER_OFF
    IF Firebase.getString(basePath + "/power_off") THEN
        rawData = firebaseData.stringData()
        IF rawData.length() > 0 THEN
            parseRawData(rawData, learnedPowerOff, learnedPowerOffLen)
            LOG("  ✓ POWER_OFF loaded (" + learnedPowerOffLen + " pulses)")
        END IF
    END IF

    // Load TEMP_UP
    IF Firebase.getString(basePath + "/temp_up") THEN
        rawData = firebaseData.stringData()
        IF rawData.length() > 0 THEN
            parseRawData(rawData, learnedTempUp, learnedTempUpLen)
            LOG("  ✓ TEMP_UP loaded (" + learnedTempUpLen + " pulses)")
        END IF
    END IF

    // Load TEMP_DOWN
    IF Firebase.getString(basePath + "/temp_down") THEN
        rawData = firebaseData.stringData()
        IF rawData.length() > 0 THEN
            parseRawData(rawData, learnedTempDown, learnedTempDownLen)
            LOG("  ✓ TEMP_DOWN loaded (" + learnedTempDownLen + " pulses)")
        END IF
    END IF

END FUNCTION
```

---

## 3.5. Thuật toán Parse Raw Data (CSV → Array)

**File**: `main.cpp` - Hàm `parseRawData()` (dòng 1995-2012)

### Pseudocode:

```
FUNCTION parseRawData(data, buffer, len):
    // Input: data = "3400,1700,400,800,..."
    // Output: buffer[] = {3400, 1700, 400, 800, ...}
    //         len = number of elements

    len = 0
    startIndex = 0

    // Parse CSV string
    WHILE startIndex < data.length() AND len < CAPTURE_BUFFER_SIZE DO
        commaIndex = data.indexOf(',', startIndex)

        IF commaIndex == -1 THEN
            // Phần tử cuối (không có dấu phẩy)
            buffer[len] = data.substring(startIndex).toInt()
            len = len + 1
            BREAK
        ELSE
            // Phần tử giữa
            buffer[len] = data.substring(startIndex, commaIndex).toInt()
            len = len + 1
            startIndex = commaIndex + 1
        END IF
    END WHILE

    RETURN

END FUNCTION
```

### Ví dụ:

```
Input string:
  data = "3400,1700,400,800,400"

Parsing process:
  Step 1: startIndex=0, commaIndex=4
    buffer[0] = "3400".toInt() = 3400
    len = 1, startIndex = 5

  Step 2: startIndex=5, commaIndex=9
    buffer[1] = "1700".toInt() = 1700
    len = 2, startIndex = 10

  Step 3: startIndex=10, commaIndex=13
    buffer[2] = "400".toInt() = 400
    len = 3, startIndex = 14

  Step 4: startIndex=14, commaIndex=17
    buffer[3] = "800".toInt() = 800
    len = 4, startIndex = 18

  Step 5: startIndex=18, commaIndex=-1 (not found)
    buffer[4] = "400".toInt() = 400
    len = 5
    BREAK

Output:
  buffer[] = {3400, 1700, 400, 800, 400}
  len = 5
```

---

## 3.6. Thuật toán Send Raw IR Signal

**File**: `main.cpp` - Hàm `sendIRLearnedCommand()` (dòng 1746-1809)

### Quy trình phát tín hiệu:

```
FUNCTION sendRawIR(buffer, len, frequency):
    // Bước 1: Tắt receiver (tránh nhiễu)
    irrecv.disableIRIn()
    DELAY(50)

    // Bước 2: Phát raw signal
    // buffer[] = {mark1, space1, mark2, space2, ...}
    // len = số phần tử
    // frequency = 38 kHz

    irsend.sendRaw(buffer, len, frequency)

    // Bước 3: Chờ hoàn tất
    DELAY(200)

    // Bước 4: Bật lại receiver
    DELAY(100)
    irrecv.enableIRIn()

END FUNCTION
```

### Cơ chế hoạt động của sendRaw():

```
┌───────────────────────────────────────────────────┐
│ IRsend::sendRaw(uint16_t buf[], uint16_t len, ...)│
└───────────────────┬───────────────────────────────┘
                    │
    ┌───────────────┴───────────────┐
    │                               │
    ▼                               ▼
┌────────────┐              ┌──────────────┐
│ Mark pulse │              │ Space (off)  │
│ PWM 38kHz  │              │ No signal    │
│ Duration:  │              │ Duration:    │
│ buf[0] µs  │              │ buf[1] µs    │
└─────┬──────┘              └──────┬───────┘
      │                            │
      └────────────┬───────────────┘
                   │
         ┌─────────▼──────────┐
         │ Repeat for all     │
         │ elements in buffer │
         └────────────────────┘

Timing precision:
  - Hardware PWM cho carrier 38kHz
  - Software timing cho mark/space duration
  - Accuracy: ±5% (đủ cho IR remote)
```

### Ví dụ phát tín hiệu:

```
Command: POWER_ON
Buffer: {3400, 1700, 400, 800, 400, 400, ...}
Length: 424 pulses

Phát tín hiệu:
  t=0µs:       Mark 3400µs at 38kHz  ████████████
  t=3400µs:    Space 1700µs (off)              ____________
  t=5100µs:    Mark 400µs at 38kHz             ██
  t=5500µs:    Space 800µs (off)                 ████
  t=6300µs:    Mark 400µs at 38kHz                   ██
  t=6700µs:    Space 400µs (off)                       ██
  ...

Total duration: ~50-100ms
```

---

## 3.7. So sánh Library Mode vs Learned Mode

| Tiêu chí | Library Mode | Learned Mode |
|----------|-------------|--------------|
| **Setup** | Chọn brand từ list | Học từng lệnh bằng remote |
| **Số lệnh** | Đầy đủ (power, temp, mode, fan, swing) | 4 lệnh cơ bản |
| **Độ tương thích** | 6 brands phổ biến | Tất cả AC có remote IR |
| **Độ chính xác** | 95-99% | 100% (raw signal) |
| **Memory usage** | ~500 bytes/protocol | ~1KB/lệnh × 4 = 4KB |
| **Tốc độ phát** | Nhanh (~50ms) | Nhanh (~50-100ms) |
| **Maintenance** | Không cần | Cần học lại nếu mất data |
| **Flexibility** | Giới hạn brands | Universal |

### Khi nào dùng mode nào?

**Library Mode**:
- AC thuộc 6 brands hỗ trợ
- Cần control đầy đủ (mode, fan, swing)
- Không muốn học từng lệnh
- Tiết kiệm memory

**Learned Mode**:
- AC không thuộc brands hỗ trợ
- Chỉ cần 4 chức năng cơ bản
- Đảm bảo 100% tương thích
- User có remote vật lý

---

## PHỤ LỤC

### A. Các hằng số quan trọng

```cpp
// IR Learning
#define CAPTURE_BUFFER_SIZE 512    // Max pulses per command
#define IR_TIMEOUT_MS 15           // IR signal timeout
const uint16_t kRawTick = 50       // Timing unit (50µs)

// Error Recovery
const int DHT_MAX_ERRORS = 2       // DHT22 error threshold
const int PZEM_MAX_ERRORS = 3      // PZEM read error threshold
const int PZEM_STUCK_THRESHOLD = 5 // PZEM stuck detection

// Timing intervals
const unsigned long SENSOR_INTERVAL = 2000   // 2s
const unsigned long STATUS_INTERVAL = 5000   // 5s
const unsigned long HEARTBEAT_INTERVAL = 10000 // 10s
const unsigned long IR_LEARNING_TIMEOUT = 10000 // 10s

// AC limits
const int AC_TEMP_MIN = 16  // °C
const int AC_TEMP_MAX = 30  // °C
const float AC_CURRENT_THRESHOLD = 0.2  // A
```

### B. Firebase paths reference

```
devices/{deviceId}/
  ├── info/
  │   ├── macAddress, firmwareVersion
  │   ├── claimed, ownerId, claimCode
  │   └── deviceName, roomName
  └── ir/
      ├── method: "library" | "learned"
      ├── library/brandId
      └── learned/commands/
          ├── power_on: "3400,1700,..."
          ├── power_off: "..."
          ├── temp_up: "..."
          └── temp_down: "..."

status/{deviceId}/
  ├── online, lastSeen
  ├── room/ (temp, humidity, hasPerson)
  └── ac/ (powerOn, setTemp, current, power)

commands/{deviceId}/
  ├── action: "power" | "tempUp" | "tempDown" | "setTemp" | "reset"
  ├── value: 16-30 (for setTemp)
  └── lastExecuted: timestamp

ir_learning/{deviceId}/
  ├── active: true/false
  ├── waiting: true/false
  ├── currentStep: 0-3
  └── result/
      ├── success: true/false
      ├── code: "raw_data_string"
      ├── protocol: "DAIKIN"
      └── bits: 312
```

---

**Kết thúc tài liệu**

> Tài liệu này mô tả chi tiết các thuật toán đang được sử dụng thực tế trong ESP32 firmware v1.0.
> Mọi pseudocode đều tương ứng với code thực tế trong file `main.cpp`.

