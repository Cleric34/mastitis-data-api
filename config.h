#pragma once

// Configuration parameters for ESP32 Mastitis Detector Firmware

#define WIFI_SSID "Yashik"
#define WIFI_PASSWORD "qwertyuiop"
#define DEVICE_ID "ESP32_01"

// Server API Endpoints (Render Deployment)
#define BASE_URL "https://mastitis-data-api.onrender.com"
#define REGISTER_COW_ENDPOINT BASE_URL "/api/v1/cows"
#define STORE_TEST_ENDPOINT   BASE_URL "/api/v1/tests"

#define CLOUD_TIMEOUT_MS 8000
#define WIFI_CONNECT_TIMEOUT_MS 25000

// I2C Pinout
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define LCD_I2C_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4

// SPI Pinout (Shared SPI Bus: SCK=18, MISO=19, MOSI=23)
#define SPI_SCK_PIN 18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23
#define SD_CS_PIN 5
#define RFID_SS_PIN 16
#define RFID_RST_PIN 17

// Analog & Sensor Pins
#define EC_PIN 34
#define PH_PIN 35
#define DS18B20_PIN 4

// Keypad Configuration
#define KEYPAD_ROW_COUNT 4
#define KEYPAD_COL_COUNT 4
