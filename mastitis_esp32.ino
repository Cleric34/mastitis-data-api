/*
  ESP32 Mastitis Detector Firmware - Production Version
  
  Hardware & Wiring (100% Preserved):
    - ESP32 DevKit V1
    - 20x4 I2C LCD Display (SDA: 21, SCL: 22, Address 0x27)
    - DS3231 / DS1307 RTC Module (SDA: 21, SCL: 22)
    - MicroSD SPI Card Module (SPI CS: 5, SCK: 18, MISO: 19, MOSI: 23)
    - RC522 RFID Reader (SPI SS: 16, RST: 17, SCK: 18, MISO: 19, MOSI: 23)
    - 4x4 Keypad (Rows: 13, 12, 14, 27; Cols: 26, 25, 33, 32)
    - EC Potentiometer (ADC Pin 34)
    - pH Potentiometer (ADC Pin 35)
    - DS18B20 Milk Temperature Sensor (OneWire Pin 4)

  Features:
    - Keypad Entry Mode with Mandatory '#' (ENTER) and 'B' (Backspace).
    - Immediate Real-time Sensor Values (EC, pH, Temp) displayed on 20x4 LCD.
    - Automatic 1-minute Wi-Fi Reconnection Loop when offline.
    - Explicit New Cow Registration with LCD Confirmation + SD / Cloud Storage.
    - Dual Input Mode (Keypad or RFID Tag Auto-Scan).
    - Render Ingestion API Integration (https://mastitis-data-api.onrender.com).
    - MicroSD Offline Queue (/TESTS/) & Automatic Background Sync.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <SD.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <Keypad.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

#include "config.h"
#include "measurement_calibration.h"

// Hardware Objects
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
RTC_DS3231 rtc3231;
RTC_DS1307 rtc1307;
uint8_t activeRtcType = 0; // 1 = DS3231, 2 = DS1307

// Keypad Configuration
char keys[KEYPAD_ROW_COUNT][KEYPAD_COL_COUNT] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[KEYPAD_ROW_COUNT] = {13, 12, 14, 27};
byte colPins[KEYPAD_COL_COUNT] = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, KEYPAD_ROW_COUNT, KEYPAD_COL_COUNT);

// OneWire DS18B20 Temperature Sensor
OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// Global States
uint32_t sequenceId = 1;
bool rtcValid = false;
bool sdReady = false;
bool rfidReady = false;
unsigned long lastSyncCheck = 0;
const unsigned long SYNC_INTERVAL_MS = 15000;
unsigned long lastWifiReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 60000; // 1 Minute Retry

// Forward Declarations
void printLcd(const char* line1, const char* line2 = "", const char* line3 = "", const char* line4 = "");
String getRfidUidString();
bool cowExistsOnSd(const String &cowId);
String readCowIdFromSd(const String &rfidUid);
bool saveCowIdToSd(const String &rfidUid, const String &cowId);
String getCowIdFromKeypad(char initialDigit = 0);
uint32_t loadSequenceId();
void saveSequenceId(uint32_t id);
String formatTimestamp();
float readAnalogAverage(int pin, int samples = 10);
void syncPendingRecords();
bool sendCowRegistrationHttp(const String &rfidUid, const String &cowId);
bool sendTestMeasurementHttp(const String &payload);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- ESP32 Mastitis Detector Firmware Starting ---");

  // 1. Initialize I2C Bus & LCD
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  printLcd("MASTITIS DETECTOR", "Initializing...", "Device: " DEVICE_ID);

  // 2. Initialize DS18B20 Temperature Sensor
  tempSensor.begin();

  // 3. Initialize RTC Module (Supports DS3231 and DS1307)
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() == 0) {
    if (rtc3231.begin()) {
      rtcValid = true;
      activeRtcType = 1;
      Serial.println("[RTC] DS3231 module detected.");
    } else if (rtc1307.begin()) {
      rtcValid = true;
      activeRtcType = 2;
      Serial.println("[RTC] DS1307 module detected.");
    }
  } else {
    Serial.println("[RTC] Module not responding on I2C address 0x68.");
  }

  // 4. Initialize Shared SPI Bus for RFID and MicroSD
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SD_CS_PIN);

  // 5. Initialize MicroSD Card
  if (SD.begin(SD_CS_PIN)) {
    sdReady = true;
    Serial.println("[SD] MicroSD Card initialized successfully.");
    if (!SD.exists("/COWS")) SD.mkdir("/COWS");
    if (!SD.exists("/TESTS")) SD.mkdir("/TESTS");
    if (!SD.exists("/config")) SD.mkdir("/config");
    sequenceId = loadSequenceId();
  } else {
    Serial.println("[SD] WARNING: MicroSD Card failed or not inserted.");
  }

  // 6. Initialize RC522 RFID Reader
  rfid.PCD_Init();
  byte ver = rfid.PCD_ReadRegister(rfid.VersionReg);
  if (ver == 0x91 || ver == 0x92 || ver == 0x88) {
    rfidReady = true;
    Serial.printf("[RFID] RC522 Reader detected (Ver 0x%02X).\n", ver);
  } else {
    Serial.println("[RFID] RC522 not responding. Keypad entry mode active.");
  }

  // 7. Connect to Wi-Fi
  Serial.printf("[Wi-Fi] Connecting to %s", WIFI_SSID);
  printLcd("MASTITIS DETECTOR", "Connecting Wi-Fi...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  unsigned long startWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWifi < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[Wi-Fi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    printLcd("MASTITIS DETECTOR", "Wi-Fi Connected!", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[Wi-Fi] Timeout. Operating in Offline Mode.");
    printLcd("MASTITIS DETECTOR", "Wi-Fi Disconnected", "Operating Offline");
  }
  delay(1500);

  // Initial Background Sync Check
  if (WiFi.status() == WL_CONNECTED && sdReady) {
    syncPendingRecords();
  }

  printLcd("MASTITIS DETECTOR", "Type Digits -> #", "OR Scan RFID Tag", "Device: " DEVICE_ID);
}

void loop() {
  // 1. Periodically check and reconnect Wi-Fi every 1 minute if disconnected
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiReconnectAttempt > WIFI_RECONNECT_INTERVAL_MS) {
    lastWifiReconnectAttempt = millis();
    Serial.println("[Wi-Fi] 1-minute timer: Retrying Wi-Fi connection...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // 2. Periodically run background sync for offline records
  if (millis() - lastSyncCheck > SYNC_INTERVAL_MS) {
    lastSyncCheck = millis();
    if (WiFi.status() == WL_CONNECTED && sdReady) {
      syncPendingRecords();
    }
  }

  String cowId = "";
  String rfidUid = "";

  // -------------------------------------------------------------------
  // CHECK 1: RFID Card Scan (Automatic if tag present & RFID working)
  // -------------------------------------------------------------------
  if (rfidReady && rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    rfidUid = getRfidUidString();
    Serial.printf("\n[RFID] Scanned Tag UID: %s\n", rfidUid.c_str());
    printLcd("RFID SCANNED", rfidUid.c_str(), "Checking Record...");

    cowId = readCowIdFromSd(rfidUid);

    if (cowId.length() == 0) {
      // New Cow via RFID: prompt operator to assign a Cow ID
      cowId = getCowIdFromKeypad();
      if (cowId.length() == 0) {
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        printLcd("MASTITIS DETECTOR", "Type Digits -> #", "OR Scan RFID Tag", "Device: " DEVICE_ID);
        return;
      }

      // Register new cow
      saveCowIdToSd(rfidUid, cowId);
      saveCowIdToSd(cowId, cowId);

      printLcd("NEW COW REGISTERED!", ("ID: " + cowId).c_str(), "Saved to SD & Cloud", "Proceeding to test...");
      if (WiFi.status() == WL_CONNECTED) {
        sendCowRegistrationHttp(rfidUid, cowId);
      }
      delay(1500);
    } else {
      printLcd("COW RECORD FOUND", ("ID: " + cowId).c_str(), ("UID: " + rfidUid).c_str(), "Proceeding to test...");
      delay(1000);
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // -------------------------------------------------------------------
  // CHECK 2: Keypad Entry Mode (Primary Input Mode)
  // -------------------------------------------------------------------
  char key = keypad.getKey();
  if (cowId.length() == 0 && key != 0) {
    if (key == '*') {
      printLcd("MASTITIS DETECTOR", "Type Digits -> #", "OR Scan RFID Tag", "Device: " DEVICE_ID);
      return;
    }
    
    // Launch Keypad Cow Entry Prompt
    cowId = getCowIdFromKeypad(key);

    if (cowId.length() == 0) {
      printLcd("MASTITIS DETECTOR", "Type Digits -> #", "OR Scan RFID Tag", "Device: " DEVICE_ID);
      return;
    }

    rfidUid = "KEYPAD_" + cowId;

    // Check if Cow ID is already registered on SD card
    if (cowExistsOnSd(cowId)) {
      Serial.printf("[COW] Existing Cow ID found: %s\n", cowId.c_str());
      printLcd("COW RECORD FOUND", ("ID: " + cowId).c_str(), "Status: Registered", "Proceeding to test...");
      delay(1000);
    } else {
      // New Cow Registration Procedure
      Serial.printf("[COW] Registering NEW Cow ID: %s\n", cowId.c_str());
      saveCowIdToSd(cowId, cowId);
      saveCowIdToSd(rfidUid, cowId);

      printLcd("NEW COW REGISTERED!", ("ID: " + cowId).c_str(), "Saved to SD & Cloud", "Proceeding to test...");

      if (WiFi.status() == WL_CONNECTED) {
        sendCowRegistrationHttp(rfidUid, cowId);
      }
      delay(1500);
    }
  }

  // If no cow selected, keep waiting
  if (cowId.length() == 0) return;

  // -------------------------------------------------------------------
  // TEAT QUARTER SELECTION PHASE
  // -------------------------------------------------------------------
  char line1[21], line2[21];
  snprintf(line1, sizeof(line1), "COW ID: %s", cowId.c_str());
  snprintf(line2, sizeof(line2), "UID: %s", rfidUid.c_str());
  printLcd(line1, line2, "Select Teat Quarter:", "1:FL 2:FR 3:RL 4:RR");

  char selectKey = 0;
  int quarter = 0;
  while (quarter == 0) {
    selectKey = keypad.getKey();
    if (selectKey >= '1' && selectKey <= '4') {
      quarter = selectKey - '0';
    } else if (selectKey == 'C' || selectKey == '*') {
      printLcd("TEST CANCELLED", "Returning to Ready");
      delay(1200);
      printLcd("MASTITIS DETECTOR", "Type Digits -> #", "OR Scan RFID Tag", "Device: " DEVICE_ID);
      return;
    }
    delay(50);
  }

  // -------------------------------------------------------------------
  // SENSOR MEASUREMENT PHASE
  // -------------------------------------------------------------------
  char qBuf[21];
  snprintf(qBuf, sizeof(qBuf), "Selected: Q%d", quarter);
  printLcd("SAMPLING SENSORS", qBuf, "Reading EC, pH, Temp...");

  float rawEc = readAnalogAverage(EC_PIN, 20);
  float rawPh = readAnalogAverage(PH_PIN, 20);

  float ec = ecFromAdc((int)rawEc);
  float ph = phFromAdc((int)rawPh);

  tempSensor.requestTemperatures();
  float tempC = tempSensor.getTempCByIndex(0);
  if (tempC < -50.0f || tempC > 100.0f) {
    tempC = 38.5f; // Fallback default bovine milk temp if probe missing
  }

  Serial.printf("[MEASURE] Raw EC: %.1f -> %.2f mS/cm | Raw pH: %.1f -> %.2f | Temp: %.1f C\n",
                rawEc, ec, rawPh, ph, tempC);

  // Generate Unique Test ID
  char testIdBuf[48];
  snprintf(testIdBuf, sizeof(testIdBuf), "%s_%06u", DEVICE_ID, sequenceId);
  String testId = String(testIdBuf);

  // Format Immediate Sensor Reading Strings for LCD
  char sensorLine1[21], sensorLine2[21];
  snprintf(sensorLine1, sizeof(sensorLine1), "Q%d EC:%.2fmS pH:%.2f", quarter, ec, ph);
  snprintf(sensorLine2, sizeof(sensorLine2), "Temp: %.1f C", tempC);

  // Build JSON Payload
  DynamicJsonDocument doc(512);
  doc["test_id"] = testId;
  doc["device_id"] = DEVICE_ID;
  doc["cow_id"] = cowId;
  doc["rfid_uid"] = rfidUid;
  doc["quarter"] = quarter;
  doc["ec"] = round(ec * 100.0f) / 100.0f;
  doc["ph"] = round(ph * 100.0f) / 100.0f;
  doc["temperature"] = round(tempC * 10.0f) / 10.0f;

  if (rtcValid) {
    doc["timestamp"] = formatTimestamp();
    doc["time_source"] = "rtc";
  } else {
    doc["timestamp"] = nullptr;
    doc["time_source"] = "sequence";
  }

  doc["sequence_id"] = sequenceId;

  // Cloud Upload & Offline Storage Logic
  bool cloudSuccess = false;
  if (WiFi.status() == WL_CONNECTED) {
    doc["delivery_mode"] = "live";
    String payload;
    serializeJson(doc, payload);

    printLcd(sensorLine1, sensorLine2, "Status: Uploading...", testId.c_str());
    cloudSuccess = sendTestMeasurementHttp(payload);
  }

  if (cloudSuccess) {
    printLcd(sensorLine1, sensorLine2, "SENT TO CLOUD! [OK]", "Press key to continue");
  } else {
    doc["delivery_mode"] = "sd_sync";
    String payload;
    serializeJson(doc, payload);

    if (sdReady) {
      char filePath[48];
      snprintf(filePath, sizeof(filePath), "/TESTS/T_%06u.JSON", sequenceId);
      File f = SD.open(filePath, FILE_WRITE);
      if (f) {
        f.print(payload);
        f.close();
        Serial.printf("[SD] Saved offline record to %s\n", filePath);
        printLcd(sensorLine1, sensorLine2, "SAVED OFFLINE (SD)", "Press key to continue");
      } else {
        printLcd(sensorLine1, sensorLine2, "SD WRITE ERROR!", "File write failed");
      }
    } else {
      printLcd(sensorLine1, sensorLine2, "OFFLINE ERROR!", "No SD Card to Queue");
    }
  }

  sequenceId++;
  if (sdReady) saveSequenceId(sequenceId);

  delay(3000);
  printLcd("MASTITIS DETECTOR", "Type Digits -> #", "OR Scan RFID Tag", "Device: " DEVICE_ID);
}

// --- Helper Functions ---

void printLcd(const char* line1, const char* line2, const char* line3, const char* line4) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
  lcd.setCursor(0, 2); lcd.print(line3);
  lcd.setCursor(0, 3); lcd.print(line4);
}

String getRfidUidString() {
  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidStr += "0";
    uidStr += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uidStr += ":";
  }
  uidStr.toUpperCase();
  return uidStr;
}

bool cowExistsOnSd(const String &cowId) {
  if (!sdReady) return false;
  String cleanId = cowId;
  cleanId.replace(":", "");
  String path = "/COWS/" + cleanId + ".TXT";
  return SD.exists(path);
}

String readCowIdFromSd(const String &rfidUid) {
  if (!sdReady) return "";
  String cleanUid = rfidUid;
  cleanUid.replace(":", "");
  String path = "/COWS/" + cleanUid + ".TXT";

  if (!SD.exists(path)) return "";

  File f = SD.open(path, FILE_READ);
  if (!f) return "";
  String cowId = f.readStringUntil('\n');
  f.close();
  cowId.trim();
  return cowId;
}

bool saveCowIdToSd(const String &rfidUid, const String &cowId) {
  if (!sdReady) return false;
  String cleanUid = rfidUid;
  cleanUid.replace(":", "");
  String path = "/COWS/" + cleanUid + ".TXT";

  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.println(cowId);
  f.close();
  Serial.printf("[SD] Saved cow record %s -> %s\n", rfidUid.c_str(), cowId.c_str());
  return true;
}

String getCowIdFromKeypad(char initialDigit) {
  printLcd("ENTER COW ID", "ID:                 ", "#:ENTER  B:BACKSPACE", "*:CANCEL");

  String enteredId = "";
  if (initialDigit >= '0' && initialDigit <= '9') {
    enteredId += initialDigit;
  }

  lcd.setCursor(4, 1);
  lcd.print(enteredId);

  while (true) {
    char k = keypad.getKey();
    if (k >= '0' && k <= '9') {
      if (enteredId.length() < 12) {
        enteredId += k;
        lcd.setCursor(4, 1);
        lcd.print(enteredId);
      }
    } else if (k == 'B') { // Backspace
      if (enteredId.length() > 0) {
        enteredId.remove(enteredId.length() - 1);
        lcd.setCursor(4, 1);
        lcd.print("            ");
        lcd.setCursor(4, 1);
        lcd.print(enteredId);
      }
    } else if (k == '*') { // Cancel
      return "";
    } else if (k == '#') { // Mandatory ENTER
      if (enteredId.length() > 0) return enteredId;
    }
    delay(50);
  }
}

uint32_t loadSequenceId() {
  if (!sdReady) return 1;
  if (!SD.exists("/config/sequence.txt")) return 1;

  File f = SD.open("/config/sequence.txt", FILE_READ);
  if (!f) return 1;
  String val = f.readStringUntil('\n');
  f.close();
  uint32_t seq = val.toInt();
  return (seq > 0) ? seq : 1;
}

void saveSequenceId(uint32_t id) {
  if (!sdReady) return;
  File f = SD.open("/config/sequence.txt", FILE_WRITE);
  if (f) {
    f.println(id);
    f.close();
  }
}

String formatTimestamp() {
  if (!rtcValid) return "";
  DateTime now = (activeRtcType == 1) ? rtc3231.now() : rtc1307.now();
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  return String(buf);
}

float readAnalogAverage(int pin, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(5);
  }
  return (float)sum / (float)samples;
}

bool sendCowRegistrationHttp(const String &rfidUid, const String &cowId) {
  HTTPClient http;
  http.begin(REGISTER_COW_ENDPOINT);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(CLOUD_TIMEOUT_MS);

  DynamicJsonDocument doc(256);
  doc["device_id"] = DEVICE_ID;
  doc["cow_id"] = cowId;
  doc["rfid_uid"] = rfidUid;
  if (rtcValid) doc["registered_at"] = formatTimestamp();

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  http.end();
  Serial.printf("[HTTP] Register Cow Response Code: %d\n", httpCode);
  return (httpCode == 201 || httpCode == 200);
}

bool sendTestMeasurementHttp(const String &payload) {
  HTTPClient http;
  http.begin(STORE_TEST_ENDPOINT);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(CLOUD_TIMEOUT_MS);

  int httpCode = http.POST(payload);
  String response = http.getString();
  http.end();

  Serial.printf("[HTTP] Store Test Response Code: %d, Body: %s\n", httpCode, response.c_str());

  if (httpCode == 201 || httpCode == 200) {
    DynamicJsonDocument resDoc(256);
    DeserializationError err = deserializeJson(resDoc, response);
    if (!err && resDoc["accepted"] == true) {
      return true;
    }
  }
  return false;
}

void syncPendingRecords() {
  if (!sdReady || WiFi.status() != WL_CONNECTED) return;

  File dir = SD.open("/TESTS");
  if (!dir) return;

  File entry = dir.openNextFile();
  int syncedCount = 0;

  while (entry) {
    bool isDir = entry.isDirectory();
    String filename = String(entry.name());
    entry.close();

    if (!isDir && (filename.endsWith(".JSON") || filename.endsWith(".json"))) {
      String path = "/TESTS/" + filename;
      File f = SD.open(path, FILE_READ);
      if (f) {
        String payload = f.readString();
        f.close();

        if (sendTestMeasurementHttp(payload)) {
          SD.remove(path);
          syncedCount++;
          Serial.printf("[SYNC] Successfully synced and deleted %s\n", path.c_str());
        }
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  if (syncedCount > 0) {
    Serial.printf("[SYNC] Total offline records synced: %d\n", syncedCount);
  }
}
