/*
** ESP32 Smart Glasses R2 - v0.22 (for 2.2k/1k divider on GPIO35)
** OLED (U8g2) + BLE (name+UUID advertised, notify enabled) + Battery monitor
*/

#define BLE 0
#define WEB_UPDATE 1

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

#define MAX_RECEIVED_ADATA_SIZE 10
#define BATTERY_GPIO 35                // ADC1_CH7 (wired via 2.2k over 1k divider)
#define EEPROM_SIZE 1
#define EEPROM_PLACE 0

// === Battery monitor settings for your divider ===
#define BATTERY_MONITOR 1              // keep ON
#define BATTERY_SCALE   3.20f          // (R1+R2)/R2 = (2.2k+1k)/1k = 3.2
#define BATTERY_CUTOFF  3.20f          // deep-sleep threshold (adjust if you want)

// ===== Includes =====
#include <EEPROM.h>
#include "driver/adc.h"
#include <esp_wifi.h>
#include <esp_bt.h>

// Disable brownout detector
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// OLED + I2C
#include <Arduino.h>
#include <U8g2lib.h>
#ifdef U8X8_HAVE_HW_SPI
  #include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
  #include <Wire.h>
#endif

// BLE
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

// WiFi / OTA
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Update.h>

// ===== I2C pins (change if needed) =====
#ifndef I2C_SDA
  #define I2C_SDA 21
#endif
#ifndef I2C_SCL
  #define I2C_SCL 22
#endif
// S3 alt: SDA=8, SCL=9   |   C3 alt: SDA=6, SCL=7

void scanI2C() {
  Serial.println("[I2C] Scanning...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  - device @ 0x%02X\n", a);
      found++;
    }
  }
  if (!found) Serial.println("  (no devices found)");
}

// ===== Globals =====
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(
  U8G2_MIRROR, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ SCL, /* data=*/ SDA
); // uses global Wire

bool shouldDrawToOled = false;
String *aOledSendData;
unsigned long powerSavePreviousMillis = 0;
unsigned long powerSaveCurrentMillis = 0;

const char* ble_name = "ESP32 (SmartGlasses)";
bool isDeviceConnected = false;

WebServer server(80);
const char* ssid = "ESP32Update";
const char* password = "12345678";

RTC_DATA_ATTR bool isDeepSleepBoot = false;

int _MarginX = 0;
int _MarginY = 0;
int _TouchSensorGPIO = 13;
int _TouchSensorThreshold = 20;
int _PowerSaveInterval = 8000;

float _BatteryVoltageFlat = BATTERY_CUTOFF; // use the #define above

// ===== Operating mode =====
int iOperatingMode = -1;

int getOperatingMode() {
  if (iOperatingMode == -1) iOperatingMode = EEPROM.read(EEPROM_PLACE);
  return iOperatingMode;
}
void setOperatingMode(int om, bool restart) {
  if (iOperatingMode == om) return;
  iOperatingMode = om;
  if (restart) {
    Serial.print("[Warning] Writing to EEPROM. Mode= "); Serial.println(om);
    EEPROM.write(EEPROM_PLACE, om); EEPROM.commit();
    delay(100);
    ESP.restart();
  }
}

// ===== Button =====
class ButtonClass {
  int lastState=0;
  long clickBreakTime=50;
  long lastClickTime=0;
  int multiClickCounter =0;
  long multiClickMaxTime = 250;
  bool longClickDetected = false;
  long longClickTime=500;
  bool shouldUpdate = false;
  int toReturn=-1;
  int samplesCurrent = 0;
  int samplesLimit = 20;
  public:
  int detect(int GPIO) {    
    long now = millis();
    int btnState = (touchRead(GPIO) < _TouchSensorThreshold) ? 1 : 0;
    if (btnState != lastState) {
      samplesCurrent++;
      if (samplesCurrent < samplesLimit) return -1;
    }
    samplesCurrent = 0;
    if (btnState != lastState && (now - lastClickTime) > clickBreakTime) {
      if (btnState == 0 && !longClickDetected) multiClickCounter++;
      lastState = btnState;
      lastClickTime=now;
      longClickDetected=false;
    }
    if ((now - lastClickTime) > longClickTime) {
      if (btnState == 1 && !longClickDetected) {
        longClickDetected=true;
        shouldUpdate=true;
      }
    }
    if (((now - lastClickTime) > multiClickMaxTime) && multiClickCounter > 0) shouldUpdate=true;

    toReturn=-1;
    if (shouldUpdate) {
      if      (longClickDetected    ) toReturn = 0;
      else if (multiClickCounter==1 ) toReturn = 1;
      else if (multiClickCounter >1 ) toReturn = multiClickCounter;
      shouldUpdate=false;
      multiClickCounter=0;
    }
    return toReturn;
  }
};

// ===== Display =====
class DisplayClass {
  public:
    int __tick = 150;
    int __previousScreen=0;
    int __currentScreen=0;
    long __lastTickTime=0;
    int __Offset=0;

    void tickScreen(String *aDataStr) {      
      __currentScreen = aDataStr[0].toInt();
      long t = millis();
      if ((t - __lastTickTime) > __tick) {
        if (__previousScreen != __currentScreen)  __Offset=0;
        switch (__currentScreen) {
          case 0: default : screenMain   (aDataStr[1]        , aDataStr[2]        , aDataStr[3]        , aDataStr[4].toInt(), aDataStr[5] ); break;
          case 1          : screenMsgNoti(aDataStr[1].toInt(), aDataStr[2]        , aDataStr[3]                                           ); break;
          case 2          : screenCall   (aDataStr[1]                                                                                     ); break;
          case 3          : screenNav    (aDataStr[1]        , aDataStr[2]        , aDataStr[3]        , aDataStr[4].toInt()              ); break;
          case 4          : screenList   (aDataStr[1].toInt(), aDataStr[2]        , aDataStr[3].toInt(), aDataStr[4]                      ); break;
          case 5          : screenMusic  (aDataStr[1].toInt(), aDataStr[2]        , aDataStr[3].toInt(), aDataStr[4].toInt()              ); break;
        }
        __previousScreen = __currentScreen;
        __lastTickTime = t;
      }
    }

    void screenMusic(int musicIcon, String title, int symbolPlayStop, int symbolNext) {
      u8g2.clearBuffer();
      drawSymbol(0, 9, musicIcon, 1);
      setFontSize(8); drawString(13, 9, title);
      drawSymbol(0 , 30, symbolPlayStop, 2);
      drawSymbol(22, 30, symbolNext, 2);
      u8g2.sendBuffer();
    }
    void screenList(int symbolMain, String title, int symbolSub, String text) {
      String l1 = text.substring(0 , 10);
      String l2 = text.substring(11, 20);
      u8g2.clearBuffer();
      drawSymbol(0, 9, symbolMain, 1); setFontSize(8 ); drawString(13, 9, title);
      drawSymbol(0, 22, symbolSub, 1);
      setFontSize(8);
      drawString(10, 21, l1);
      drawString(10, 30, l2);
      u8g2.sendBuffer();
    }
    void screenNav(String maxSpeed, String distance, String distanceToDes, int symbol) {
      u8g2.clearBuffer();
      drawSymbol(0, 22, symbol, 2);
      setFontSize(7 ); drawString(18, 7, maxSpeed);
      setFontSize(12); drawString(21, 21, distance);
      setFontSize(7 ); drawString(18, 31, distanceToDes);
      u8g2.sendBuffer();
    }
    void screenCall(String from) {
      if (__Offset >= 6) __Offset=0;
      u8g2.clearBuffer();
      drawSymbol(0, 9, 260, 1);
      setFontSize(9);
      if      (__Offset == 0) drawString(12, 9, "Calling.");
      else if (__Offset == 1) drawString(12, 9, "Calling..");
      else                    drawString(12, 9, "Calling...");
      setFontSize(8); drawString(0, 22, from);
      u8g2.sendBuffer();
      __Offset++;
    }
    void screenMsgNoti(int symbol, String from, String text) {
      int d_width = u8g2.getDisplayWidth();
      String cut = text;
      if (__Offset >= text.length() ) __Offset=0;
      u8g2.clearBuffer();
      drawSymbol(0, 9, symbol, 1);
      setFontSize(8 ); drawString(13, 9, from);
      setFontSize(10);
      cut = text.substring(0 + __Offset, d_width - 108 + __Offset);
      drawString(0, 25, cut);
      u8g2.sendBuffer();
      __Offset++;
    }
    void screenMain(String HH, String mm, String date, int symbol, String degrees) {
      int addDePx = (degrees.length() < 3) ? 4 : 0;
      if (__Offset >= 10) __Offset=0;
      u8g2.clearBuffer();
      setFontSize(12);
      drawString(0, 17, HH);
      if (__Offset < 4) drawString(20, 17, ":");
      drawString(25, 17, mm);
      setFontSize(8 ); drawString(4, 29, date);
      drawSymbol(52, 22, symbol, 2);
      setFontSize(7 ); drawString(50 + addDePx, 30, degrees);
      u8g2.sendBuffer();
      __Offset++;
    }

    void drawSymbol(int x, int y, int index, int size) {
      switch (size) {
        case 1: u8g2.setFont(u8g2_font_open_iconic_all_1x_t ); break;
        case 2: u8g2.setFont(u8g2_font_open_iconic_all_2x_t ); break;
        case 4: u8g2.setFont(u8g2_font_open_iconic_all_4x_t ); break;
        case 6: u8g2.setFont(u8g2_font_open_iconic_all_6x_t ); break;
        case 8: u8g2.setFont(u8g2_font_open_iconic_all_8x_t ); break;
        default: u8g2.setFont(u8g2_font_open_iconic_all_1x_t ); break;
      }
      u8g2.drawGlyph(_MarginX+x, _MarginY+y, index);
    }
    int getStringWidth(String text) {
      int t_width = 0; String s = "";
      for (int i = 0; i < text.length(); i++) {
        s = text.charAt(i);
        t_width += u8g2.getStrWidth(s.c_str()) + 1;
      }
      return t_width;
    }
    void setFontSize(int size) {
      switch (size) {
        case 4 : u8g2.setFont(u8g2_font_u8glib_4_tr)      ; break;
        case 5 : u8g2.setFont(u8g2_font_micro_tr)         ; break;
        case 6 : u8g2.setFont(u8g2_font_5x8_tr)           ; break;
        case 7 : u8g2.setFont(u8g2_font_profont11_tr)     ; break;
        case 8 : u8g2.setFont(u8g2_font_profont12_tr)     ; break;
        case 9 : u8g2.setFont(u8g2_font_t0_14_tr)         ; break;
        case 10: u8g2.setFont(u8g2_font_unifont_tr)       ; break;
        case 12: u8g2.setFont(u8g2_font_samim_16_t_all)   ; break;
        case 18: u8g2.setFont(u8g2_font_ncenR18_tr)       ; break;
        default: u8g2.setFont(u8g2_font_5x8_tr)           ; break;
      }
    }
    void drawString(int x, int y, String text) { u8g2.drawStr(_MarginX+x, _MarginY+y, text.c_str()); }
    void sendBuffer() { u8g2.sendBuffer(); }
    void clearBuffer() { u8g2.clearBuffer(); }
    void clear() { u8g2.clear(); }

    int lastPowerSaveMode = -1;
    bool setPowerSave(int i) {
      bool changed=false;
      if (lastPowerSaveMode != i) {
        if (i == 1) Serial.println("[INFO - OLED] Power save mode");
        u8g2.setPowerSave(i);
        changed = true;
      }
      lastPowerSaveMode = i;
      return changed;
    }
};

// Forward declaration so BLE can print to OLED
extern DisplayClass dc;

// ===== BLE helpers =====
String getBleMacString() { return String(BLEDevice::getAddress().toString().c_str()); }

/* --- BLE - Receive --- */
class BLEReceive {
  public:
    class BLEConnectState : public BLEServerCallbacks {
        void onConnect(BLEServer* pServer) {
          isDeviceConnected = true;
          Serial.println("[INFO - BLE] Device connected");
        }
        void onDisconnect(BLEServer* pServer) {
          isDeviceConnected = false;
          Serial.println("[INFO - BLE] Device disconnected");
          delay(100);
          ESP.restart();
        }
    };

    class BLEReceiveClass : public BLECharacteristicCallbacks {
        String aReceivedData[MAX_RECEIVED_ADATA_SIZE];
        int indexRD = 0;

        void clearReceivedData() {
          for (int i = 0; i < MAX_RECEIVED_ADATA_SIZE; i++) aReceivedData[i] = "";
          indexRD = 0;
        }

        void onWrite(BLECharacteristic *pCharacteristic) {
          String sReceived = "";
          auto rv = pCharacteristic->getValue();
          if (rv.length() > 0) {
            sReceived = String(rv.c_str());
            Serial.print("[INFO - BLE] Received: "); Serial.println(sReceived);
          } else return;

          if      (sReceived == "#OM=0"   )  { setOperatingMode(BLE        , true); return; }
          else if (sReceived == "#OM=1"   )  { setOperatingMode(WEB_UPDATE , true); return; }
          else if (sReceived == "#RESTART")  { ESP.restart()                      ; return; }

          else if (sReceived.startsWith("#MX="  )) { _MarginX              = sReceived.substring(4).toInt(); Serial.println(_MarginX);              return; }
          else if (sReceived.startsWith("#MY="  )) { _MarginY              = sReceived.substring(4).toInt(); Serial.println(_MarginY);              return; }
          else if (sReceived.startsWith("#PSI=" )) { _PowerSaveInterval    = sReceived.substring(5).toInt(); Serial.println(_PowerSaveInterval);    return; }
          else if (sReceived.startsWith("#TSG=" )) { _TouchSensorGPIO      = sReceived.substring(5).toInt(); Serial.println(_TouchSensorThreshold); return; }
          else if (sReceived.startsWith("#TST=" )) { _TouchSensorThreshold = sReceived.substring(5).toInt(); Serial.println(_TouchSensorThreshold); return; }
          else if (sReceived.startsWith("#BF="  )) { _BatteryVoltageFlat   = sReceived.substring(4).toFloat(); Serial.println(_BatteryVoltageFlat); return; }

          if (sReceived.startsWith("#")) {
            sReceived = sReceived.substring(1);
            clearReceivedData();
            for (int i = 0; i < sReceived.length(); i++) {
              if (indexRD >= MAX_RECEIVED_ADATA_SIZE) break;
              if (sReceived.charAt(i) == '|') { indexRD++; continue; }
              aReceivedData[indexRD] += sReceived.charAt(i);
            }
            aOledSendData = aReceivedData;
            shouldDrawToOled = true;
            powerSavePreviousMillis = millis();
          }
        }
    };

    BLECharacteristic *pCharacteristic;

    void sendValue(String value) {
      pCharacteristic->setValue(value.c_str());
      pCharacteristic->notify();
      delay(30);
    }

    void init() {
      BLEDevice::init(ble_name);

      String mac = getBleMacString();
      Serial.print("[INFO - BLE] Starting with MAC address: "); Serial.println(mac);
      Serial.println("[WARNING!] Copy this MAC when pairing.");

      dc.setPowerSave(0);
      dc.setFontSize(7);  dc.drawString(0, 10, "BLE MAC:");
      dc.setFontSize(6);  dc.drawString(0, 20, mac);
      dc.sendBuffer();

      BLEServer *pServer = BLEDevice::createServer();
      pServer->setCallbacks(new BLEConnectState());

      BLEService *pService = pServer->createService(SERVICE_UUID);

      pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
      );
      pCharacteristic->setCallbacks(new BLEReceiveClass());
      pCharacteristic->addDescriptor(new BLE2902());
      pCharacteristic->setValue("1");

      pService->start();

      BLEAdvertising *adv = BLEDevice::getAdvertising();
      adv->addServiceUUID(SERVICE_UUID);
      adv->setScanResponse(true);
      adv->setMinPreferred(0x06);
      adv->setMinPreferred(0x12);
      BLEDevice::startAdvertising();

      Serial.println("[INFO - BLE] Advertising started. Look for: " + String(ble_name));
    }
};

// ===== WebUpdate (unchanged core) =====
const char* serverIndex =
  "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
  "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
  "<input type='file' name='update'>"
  "<input type='submit' value='Update'>"
  "</form>"
  "<div><a href='/ble'>Go back to BLE</a></div>"
  "<div><a href='/mac'>Check MAC Address</a></div>"
  "<div id='prg'>progress: 0%</div>"
  "<script>"
  "$('form').submit(function(e){"
  "e.preventDefault();"
  "var form = $('#upload_form')[0];"
  "var data = new FormData(form);"
  " $.ajax({"
  "url: '/update',"
  "type: 'POST',"
  "data: data,"
  "contentType: false,"
  "processData:false,"
  "xhr: function() {"
  "var xhr = new window.XMLHttpRequest();"
  "xhr.upload.addEventListener('progress', function(evt) {"
  "if (evt.lengthComputable) {"
  "var per = evt.loaded / evt.total;"
  "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
  "}"
  "}, false);"
  "return xhr;"
  "},"
  "success:function(d, s) {"
  "console.log('success!')"
  "},"
  "error: function (a, b, c) {"
  "}"
  "});"
  "});"
  "</script>";

class WebUpdate {
  public:
    void init() {
      WiFi.begin(ssid, password);
      for (int i = 0; WiFi.status() != WL_CONNECTED; i++) {
        if (i >= 50) { setOperatingMode(BLE, true); break; }
        delay(500);
        Serial.print(".");
      }
      Serial.println("");
      Serial.print("[INFO - WebUpdate] Connected. IP address:");
      Serial.println(WiFi.localIP());

      server.on("/", HTTP_GET, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/html", serverIndex);
      });
      server.on("/ble", []() {
        server.send(200, "text/plain", "OK - Turning on BLE");
        setOperatingMode(BLE, true);
      });
      server.on("/mac", []() {
        server.send(200, "text/plain", "MAC Address: " + WiFi.macAddress());
      });
      server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart();
      }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          Serial.printf("Update: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
            Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
        }
      });
      server.begin();
    }
};

// ===== Singletons =====
DisplayClass dc;
BLEReceive ble;
ButtonClass button;

// ===== Battery helpers =====
static float lastVoltage = 0.0f;
static long lastTimeCheck=0;
static const long checkTime=2000; // ms

float readBatteryVoltsAveraged() {
  // proper attenuation for up to ~3.3V at pin (your pin sees ~1.0–1.4V)
  analogSetPinAttenuation(BATTERY_GPIO, ADC_11db);

  // average multiple samples to reduce noise
  const int N = 16;
  long sum_mV = 0;
#if ARDUINO_ESP32_MAJOR >= 2
  for (int i=0;i<N;i++) sum_mV += analogReadMilliVolts(BATTERY_GPIO);
  float v_pin = (sum_mV / (float)N) / 1000.0f; // volts at ADC pin
#else
  for (int i=0;i<N;i++) sum_mV += analogRead(BATTERY_GPIO);
  float raw = sum_mV / (float)N;               // 0..4095
  float v_pin = (raw / 4095.0f) * 3.3f;        // approx volts at ADC pin
#endif
  return v_pin * BATTERY_SCALE;                 // real battery volts
}

// 0 = flat, 1 = OK
int getBatteryStatus() {
  if (!BATTERY_MONITOR) return 1;
  if ((millis() - lastTimeCheck) < checkTime) return 1;

  float v_batt = readBatteryVoltsAveraged();
  lastVoltage = v_batt;
  lastTimeCheck = millis();

  // Also print raw info once per interval
#if ARDUINO_ESP32_MAJOR >= 2
  // we already averaged in mV; nothing more to print raw
  Serial.print("[INFO] Battery voltage: ");
  Serial.print(v_batt, 2);
  Serial.println(" V");
#else
  Serial.print("[INFO] Battery voltage (approx): ");
  Serial.print(v_batt, 2);
  Serial.println(" V");
#endif

  if (v_batt <= _BatteryVoltageFlat) return 0;
  return 1;
}

void prepareToDeepSleep() {
  dc.setPowerSave(1);

  if (getOperatingMode() == WEB_UPDATE) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
  } else {
    btStop();
    esp_bt_controller_disable();
  }

  // adc_power_off() removed in newer cores; keep guarded
  #if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR < 4)
    adc_power_off();
  #endif
}

void oledPowerSave() {
  powerSaveCurrentMillis = millis();
  if ((powerSaveCurrentMillis - powerSavePreviousMillis) >= _PowerSaveInterval) {
    bool changed = dc.setPowerSave(1); delay(3);
    shouldDrawToOled = false;
    if (changed && getOperatingMode() == BLE) ble.sendValue("1");
    powerSavePreviousMillis = powerSaveCurrentMillis;
  }
}

void goDeepSleep(int seconds) {
  isDeepSleepBoot = true;
  esp_sleep_enable_timer_wakeup(seconds * 1000000LL);
  Serial.println("[WARNING!] Entering deep sleep ...");
  esp_deep_sleep_start();
}

// ===== Setup / Loop =====
void setup() {
  Serial.begin(115200);
  pinMode(BATTERY_GPIO, INPUT);

  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_PLACE) != 0 && EEPROM.read(EEPROM_PLACE) != 1) {
    EEPROM.write(EEPROM_PLACE, BLE);
    EEPROM.commit();
    Serial.println("[INFO] EEPROM init done!");
  }

  Serial.println("[INFO] Version: 0.22");
  Serial.println("[WARNING] Disable brownout detector");
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  delay(3000);  // some OLEDs like a longer cold-start

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  scanI2C();
  u8g2.setI2CAddress(0x3C << 1); // change to (0x3D << 1) if needed
  u8g2.begin();
  u8g2.setPowerSave(0);
  Wire.setClock(400000);

  if (getOperatingMode() == WEB_UPDATE) {
    Serial.println("[INFO] OperatingMode: WEB_UPDATE");
    WebUpdate webUpdate; webUpdate.init();
  } else {
    Serial.println("[INFO] OperatingMode: BLE");
    ble.init();
  }

  // OLED banner
  delay(100);
  dc.setFontSize(7);
  dc.drawString(10, 10, "Ready! Mode:");
  dc.drawString(10, 20, (getOperatingMode() == WEB_UPDATE) ? "WU" : "BLE");
  dc.sendBuffer();
  powerSavePreviousMillis = millis();
}

void loop() {
  // Battery guard
  if (getBatteryStatus() == 0) {
    Serial.print("[WARNING] Battery low (<= ");
    Serial.print(_BatteryVoltageFlat, 2);
    Serial.print(" V). Measured: ");
    Serial.print(lastVoltage, 2);
    Serial.println(" V. Entering deep sleep.");
    dc.setFontSize(7); dc.drawString(20, 30, "LOW BAT!"); dc.sendBuffer();
    delay(2000);
    prepareToDeepSleep();
    delay(100);
    esp_deep_sleep_start();
    return;
  }

  // OLED power save
  oledPowerSave();

  if (getOperatingMode() == WEB_UPDATE) {
    server.handleClient();
    delay(1);
  } else {
    int action = button.detect(_TouchSensorGPIO);
    if (action > -1) {
      if      (action == 0) { Serial.println("[INFO - TouchSensor1] Detected long click!"); ble.sendValue("#TS0"); }
      else if (action  > 0) { Serial.print  ("[INFO - TouchSensor1] Detected click x "); Serial.println(action); ble.sendValue("#TS"+String(action)); }
    }
    if (shouldDrawToOled) {
      dc.setPowerSave(0); delay(3);
      dc.tickScreen(aOledSendData);
    }
  }
}
